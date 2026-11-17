"""Exercise file SQL against an isolated seekdb instance; Python standard library only."""
import argparse
import base64
from contextlib import closing
import hashlib
import json
from pathlib import Path
import secrets
import shutil
import socket
import struct
import tempfile

class SqlError(RuntimeError):
    def __init__(self, code, message):
        super().__init__(f'{code}: {message}')
        self.code = code

class PipeTransport:
    def __init__(self, name):
        self.file = open('\\\\.\\pipe\\'+name, 'r+b', buffering=0)
    def recv(self, size):
        return self.file.read(size)
    def sendall(self, data):
        offset = 0
        while offset < len(data):
            offset += self.file.write(data[offset:])
    def close(self):
        self.file.close()

class Client:
    def __init__(self, host, port, user='root', pipe=None):
        self.socket = PipeTransport(pipe) if pipe else socket.create_connection((host,port),timeout=10)
        if not pipe:
            self.socket.settimeout(60)
        greeting = self.receive()
        if not greeting or greeting[0] != 10:
            raise RuntimeError('unexpected MySQL greeting')
        flags = 1 | 4 | 512 | 8192 | 32768 | 131072 | 524288
        payload = struct.pack('<IIB',flags,16*1024*1024,45)+bytes(23)+user.encode()+b'\0\0mysql_native_password\0'
        self.send(payload,1)
        response = self.receive()
        if response[:1] == b'\xfe':
            self.send(b'',3)
            response = self.receive()
        self.check_error(response)
        if response[:1] != b'\x00':
            raise RuntimeError('this local test client requires an empty-password test account')
    def close(self):
        self.socket.close()
    def exact(self, size):
        data = bytearray()
        while len(data) < size:
            part = self.socket.recv(size-len(data))
            if not part:
                raise EOFError('server closed the connection')
            data += part
        return bytes(data)
    def receive(self):
        header = self.exact(4)
        size = int.from_bytes(header[:3],'little')
        if size >= 16*1024*1024:
            raise RuntimeError('test response exceeded the packet limit')
        return self.exact(size)
    def send(self, data, sequence=0):
        self.socket.sendall(len(data).to_bytes(3,'little')+bytes([sequence])+data)
    @staticmethod
    def check_error(packet):
        if packet[:1] == b'\xff':
            code = int.from_bytes(packet[1:3],'little')
            start = 9 if packet[3:4] == b'#' else 3
            raise SqlError(code,packet[start:].decode('utf-8','replace'))
    @staticmethod
    def value(packet, offset):
        tag = packet[offset]
        offset += 1
        if tag == 251:
            return None,offset
        if tag < 251:
            size = tag
        else:
            width = {252:2,253:3,254:8}.get(tag)
            if width is None:
                raise RuntimeError('invalid result length')
            size = int.from_bytes(packet[offset:offset+width],'little')
            offset += width
        if size > len(packet)-offset:
            raise RuntimeError('truncated result value')
        return packet[offset:offset+size],offset+size
    def query(self, sql):
        self.send(b'\x03'+sql.encode('utf-8'))
        packet = self.receive()
        self.check_error(packet)
        if packet[:1] == b'\x00':
            return [],[]
        count = packet[0]
        if count >= 251:
            raise RuntimeError('too many columns for the test client')
        columns = []
        for _ in range(count):
            definition = self.receive()
            offset = 0
            fields = []
            for _ in range(6):
                value,offset = self.value(definition,offset)
                fields.append(value)
            columns.append(fields[4].decode('utf-8'))
        end = self.receive()
        self.check_error(end)
        if end[:1] != b'\xfe':
            raise RuntimeError('expected column EOF')
        rows = []
        while True:
            packet = self.receive()
            self.check_error(packet)
            if packet[:1] == b'\xfe' and len(packet) < 9:
                break
            row = []
            offset = 0
            for _ in range(count):
                value,offset = self.value(packet,offset)
                row.append(None if value is None else value.decode('utf-8'))
            rows.append(tuple(row))
        return columns,rows


def literal(text):
    # Paths in this test use forward slashes; reject quoting ambiguity explicitly.
    if "'" in text or '\\' in text or '\0' in text:
        raise ValueError('unsupported test literal')
    return "'"+text+"'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host',default='127.0.0.1')
    parser.add_argument('--port',required=True,type=int)
    parser.add_argument('--user',default='root')
    parser.add_argument('--pipe-file',type=Path,help='Windows run/sql.pipe discovery file; required for secure_file_priv setup')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[4]
    asset = root/'tools/deploy/mysql_test/test_suite/file_sql/data'
    pipe = args.pipe_file.read_text().strip() if args.pipe_file else None
    connect = lambda: Client(args.host,args.port,args.user,pipe)
    with tempfile.TemporaryDirectory(prefix='seekdb-file-sql-outside-') as outside, tempfile.TemporaryDirectory(prefix='seekdb-file-sql-') as scratch:
        scratch = Path(scratch)
        outside_file = Path(outside)/'outside.csv'
        outside_file.write_text('id\n999\n',encoding='utf-8')
        for name in ['basic.csv','basic.jsonl']:
            shutil.copyfile(asset/name,scratch/name)
        (scratch/'simple.parquet').write_bytes(base64.b64decode((asset/'simple.parquet.b64').read_bytes()))
        shutil.copyfile(root/'unittest/sql/engine/basic/fixtures/mixed_snappy.parquet',scratch/'mixed.parquet')
        (scratch/'bad.csv').write_text('id,name\n1,"unterminated',encoding='utf-8')
        (scratch/'nested.jsonl').write_text('{"x":[]}\n',encoding='utf-8')
        (scratch/'options.csv').write_text("a;b\n1;NONE\n2;'two;parts'\n",encoding='utf-8')
        (scratch/'fraction.csv').write_text('stamp\n2026-08-02 12:13:14.123456\n',encoding='utf-8')
        path = lambda name: literal((scratch/name).as_posix())
        setup = connect()
        old_root = setup.query('SELECT @@global.secure_file_priv')[1][0][0]
        failures = []
        checks = 0
        def check(client, name, sql, expected):
            nonlocal checks
            print('SQL:',sql,flush=True)
            actual = client.query(sql)[1]
            if actual != expected:
                raise AssertionError(f'{name}: expected {expected!r}, got {actual!r}')
            checks += 1
            print('PASS:',name,flush=True)
        def error(client, name, sql, code=None):
            nonlocal checks
            print('SQL:',sql,flush=True)
            try:
                client.query(sql)
            except SqlError as result:
                if code is not None and result.code != code:
                    raise AssertionError(f'{name}: wrong error {result}')
                checks += 1
                print('PASS:',name,'error',result.code,flush=True)
                return
            raise AssertionError(f'{name}: query unexpectedly succeeded')
        try:
            check(setup,'ordinary SQL','SELECT 1+1',[('2',)])
            check(setup,'default disabled','SELECT @@session.enable_file_sql',[('0',)])
            error(setup,'disabled gate',f'SELECT count(*) FROM read_csv({path("basic.csv")})',1235)
            setup.query('SET GLOBAL secure_file_priv='+literal(scratch.as_posix()))
            setup.close()
            with closing(connect()) as client:
                client.query('SET SESSION enable_file_sql=ON')
                check(client,'CSV aggregate',f'SELECT count(*),sum(id),round(avg(score),2),sum(active) FROM read_csv({path("basic.csv")})',[('3','6','91.92','2')])
                check(client,'CSV duplicate headers, multiline and dates',f"SELECT id,id__2,day,created,replace(note,char(10),'<NL>') FROM read_csv({path('basic.csv')}) ORDER BY id",[('1','101','2026-08-01','2026-08-01 10:11:12','hello,<NL>world'),('2','102',None,None,'plain'),('3','103','2026-08-03','2026-08-03 12:13:14','say \"hi\"')])
                check(client,'projection/filter/order/limit',f'SELECT name,id+10 FROM read_csv({path("basic.csv")}) f WHERE score>90 ORDER BY id DESC LIMIT 1',[('Chen','13')])
                check(client,'named options and full inference',f'SELECT count(*) FROM read_csv({path("basic.csv")},header=true,sample_size=-1)',[('3',)])
                check(client,'CSV custom syntax',f"SELECT a,b FROM read_csv({path('options.csv')},delimiter=';',quote='''',escape='''',nullstr='NONE') ORDER BY a",[('1',None),('2','two;parts')])
                check(client,'fractional datetime',f'SELECT stamp FROM read_csv({path("fraction.csv")})',[('2026-08-02 12:13:14.123456',)])
                check(client,'group and having',f'SELECT active,count(*) FROM read_csv({path("basic.csv")}) GROUP BY active HAVING count(*)>1',[('1','2')])
                check(client,'JSONL union',f'SELECT id,name,score,day,note FROM read_ndjson({path("basic.jsonl")}) ORDER BY id',[('1','Alice','98.5',None,None),('2','Bob',None,'2026-08-02',None),('3','Chen','90',None,'ok')])
                check(client,'cross-format inner join',f'SELECT c.id,c.name,j.note FROM read_csv({path("basic.csv")}) c JOIN read_json({path("basic.jsonl")}) j ON c.id=j.id ORDER BY c.id',[('1','Alice',None),('2','Bob',None),('3','Chen','ok')])
                check(client,'left join',f'SELECT c.id,j.name FROM read_csv({path("basic.csv")}) c LEFT JOIN read_json({path("basic.jsonl")}) j ON c.id=j.id AND j.id=1 ORDER BY c.id',[('1','Alice'),('2',None),('3',None)])
                check(client,'Parquet count and values',f'SELECT count(*),min(c1),max(c1),sum(c1) FROM read_parquet({path("simple.parquet")})',[('7','0','6','21')])
                check(client,'Parquet row groups/dictionary/NULLs',f'SELECT count(*),sum(id),count(score) FROM read_parquet({path("mixed.parquet")})',[('6','13','5')])
                check(client,'glob',f'SELECT count(*) FROM glob({path("*.parquet")})',[('2',)])
                columns,description = client.query(f'DESCRIBE SELECT * FROM read_csv({path("basic.csv")})')
                if columns != ['column_name','column_type','null','key','default','extra'] or description[0][:3] != ('id','BIGINT','NO') or len(description) != 8:
                    raise AssertionError(f'DESCRIBE mismatch: {columns!r} {description!r}')
                checks += 1
                print('PASS: DESCRIBE',flush=True)
                client.query(f'COPY (SELECT id,name,score FROM read_csv({path("basic.csv")}) ORDER BY id) TO {path("export.csv")} (FORMAT CSV,HEADER,NULLSTR \'NULL\')')
                check(client,'COPY read back',f'SELECT id,name,score FROM read_csv({path("export.csv")}) ORDER BY id',[('1','Alice','98.5'),('2','Bob','87'),('3','Chen','90.25')])
                error(client,'COPY refuses overwrite',f'COPY (SELECT 1) TO {path("export.csv")}')
                error(client,'COPY runtime error',f"COPY (SELECT JSON_EXTRACT(name,'$') FROM read_csv({path('basic.csv')})) TO {path('failed.csv')}")
                if (scratch/'failed.csv').exists() or any('.tmp' in p.name for p in scratch.iterdir()):
                    raise AssertionError('failed COPY left output or temporary files')
                (scratch/'refresh.csv').write_text('first\n1\n',encoding='utf-8')
                refresh_sql = f'SELECT * FROM read_csv({path("refresh.csv")})'
                check(client,'initial inferred schema',refresh_sql,[('1',)])
                (scratch/'refresh.csv').write_text('second,third\n2,3\n',encoding='utf-8')
                check(client,'schema refresh for repeated SQL',refresh_sql,[('2','3')])
                database = 'file_sql_check_'+secrets.token_hex(6)
                client.query('CREATE DATABASE '+database)
                try:
                    client.query('CREATE TABLE '+database+'.sample (id BIGINT, name VARCHAR(30))')
                    client.query("INSERT INTO "+database+".sample VALUES (7,'saved')")
                    client.query(f'COPY {database}.sample TO {path("table.csv")} (HEADER)')
                    check(client,'COPY table',f'SELECT * FROM read_csv({path("table.csv")})',[('7','saved')])
                finally:
                    client.query('DROP DATABASE '+database)
                error(client,'malformed CSV',f'SELECT * FROM read_csv({path("bad.csv")})')
                error(client,'nested JSON',f'SELECT * FROM read_json({path("nested.jsonl")})')
                error(client,'sample-size restriction',f'SELECT * FROM read_csv({path("basic.csv")},sample_size=1)',1235)
                error(client,'secure root escape',f'SELECT * FROM read_csv({literal(outside_file.as_posix())})')
                client.query('SET SESSION enable_file_sql=OFF')
                check(client,'ordinary SQL after file queries','SELECT 2*3',[('6',)])
        finally:
            try:
                setup.close()
                with closing(connect()) as restore:
                    restore.query('SET GLOBAL secure_file_priv='+('NULL' if old_root is None else literal(old_root.replace('\\','/'))))
            except Exception as cleanup_error:
                print('CLEANUP ERROR:',cleanup_error,flush=True)
        print(json.dumps({'passed_checks':checks,'result':'passed'}),flush=True)

if __name__ == '__main__':
    main()
