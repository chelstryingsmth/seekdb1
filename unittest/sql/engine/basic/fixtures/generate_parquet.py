"""Generate small standard Parquet fixtures without a query engine dependency."""
from pathlib import Path
import struct

BOOL, I32, I64, BINARY, LIST, STRUCT = 1, 5, 6, 8, 9, 12

def vint(number):
    output = bytearray()
    while number >= 128:
        output.append((number & 127) | 128)
        number >>= 7
    return bytes(output + bytes([number]))

def encode(kind, value):
    if kind in (I32,I64):
        return vint((value << 1) ^ (value >> 63))
    if kind == BINARY:
        value = value.encode() if isinstance(value,str) else value
        return vint(len(value)) + value
    if kind == STRUCT:
        return structure(value)
    if kind == LIST:
        element, values = value
        header = bytes([(min(15,len(values)) << 4) | element])
        if len(values) >= 15:
            header += vint(len(values))
        return header + b''.join(encode(element,item) for item in values)
    raise ValueError(kind)

def structure(fields):
    output = bytearray()
    previous = 0
    for index, kind, value in fields:
        actual = (1 if value else 2) if kind == BOOL else kind
        delta = index-previous
        output.append(((delta << 4) if 0 < delta < 16 else 0) | actual)
        if not 0 < delta < 16:
            output.extend(encode(I32,index))
        if kind != BOOL:
            output.extend(encode(kind,value))
        previous = index
    return bytes(output + b'\x00')

def snappy_literal(data):
    n = len(data)
    if n == 0:
        return b'\x00'
    length = n-1
    if length < 60:
        header = bytes([length << 2])
    else:
        size = (length.bit_length()+7)//8
        header = bytes([(59+size) << 2]) + length.to_bytes(size,'little')
    return vint(n) + header + data

def plain(kind, values):
    if kind == 0:
        return bytes([sum(bool(value) << i for i,value in enumerate(values))]) if values else b''
    if kind in (1,2,5):
        code = {1:'i',2:'q',5:'d'}[kind]
        return b''.join(struct.pack('<'+code,value) for value in values)
    return b''.join(struct.pack('<I',len(value.encode()))+value.encode() for value in values)

def fixture(destination):
    columns = [('id',2,None),('name',6,0),('active',0,None),('score',5,None),('day',1,6),('created',2,10)]
    groups = [
        [[1,None,3],['A','B','A'],[True,False,None],[1.25,None,2.5],[0,None,1],[0,1234567,None]],
        [[4,5,None],['C','C','D'],[False,True,True],[3.5,4.5,5.5],[2,3,4],[2000000,None,3000000]],
    ]
    output = bytearray(b'PAR1')
    row_groups = []
    for group_index, values in enumerate(groups):
        chunks = []
        total = 0
        for (name,physical,converted), rows in zip(columns,values):
            offset = len(output)
            uncompressed_size = 0
            present = [value for value in rows if value is not None]
            definitions = b''.join(vint(2)+bytes([int(value is not None)]) for value in rows)
            dictionary_offset = None
            encoding = 0
            if name == 'name':
                dictionary = list(dict.fromkeys(present))
                raw = plain(physical,dictionary)
                compressed = snappy_literal(raw)
                header = structure([(1,I32,2),(2,I32,len(raw)),(3,I32,len(compressed)),(7,STRUCT,[(1,I32,len(dictionary)),(2,I32,0)])])
                dictionary_offset = len(output)
                output.extend(header+compressed)
                uncompressed_size += len(header)+len(raw)
                data = bytes([1]) + b''.join(vint(2)+bytes([dictionary.index(value)]) for value in present)
                encoding = 8
            else:
                data = plain(physical,present)
            data_offset = len(output)
            if group_index == 0:
                raw = struct.pack('<I',len(definitions))+definitions+data
                body = snappy_literal(raw)
                details = [(1,I32,len(rows)),(2,I32,encoding),(3,I32,3),(4,I32,3)]
                header = structure([(1,I32,0),(2,I32,len(raw)),(3,I32,len(body)),(5,STRUCT,details)])
            else:
                raw = definitions+data
                body = definitions+snappy_literal(data)
                details = [(1,I32,len(rows)),(2,I32,len(rows)-len(present)),(3,I32,len(rows)),(4,I32,encoding),(5,I32,len(definitions)),(6,I32,0),(7,BOOL,True)]
                header = structure([(1,I32,3),(2,I32,len(raw)),(3,I32,len(body)),(8,STRUCT,details)])
            output.extend(header+body)
            size = len(output)-offset
            uncompressed_size += len(header)+len(raw)
            total += uncompressed_size
            metadata = [(1,I32,physical),(2,LIST,(I32,[0,3,8])),(3,LIST,(BINARY,[name])),(4,I32,1),(5,I64,len(rows)),(6,I64,uncompressed_size),(7,I64,size),(9,I64,data_offset)]
            if dictionary_offset is not None:
                metadata.append((11,I64,dictionary_offset))
            chunks.append([(2,I64,offset),(3,STRUCT,metadata)])
        row_groups.append([(1,LIST,(STRUCT,chunks)),(2,I64,total),(3,I64,3)])
    schema = [[(4,BINARY,'schema'),(5,I32,len(columns))]]
    for name,physical,converted in columns:
        column = [(1,I32,physical),(3,I32,1),(4,BINARY,name)]
        if converted is not None:
            column.append((6,I32,converted))
        schema.append(column)
    footer = structure([(1,I32,1),(2,LIST,(STRUCT,schema)),(3,I64,6),(4,LIST,(STRUCT,row_groups))])
    destination.write_bytes(output+footer+struct.pack('<I',len(footer))+b'PAR1')

if __name__ == '__main__':
    fixture(Path(__file__).with_name('mixed_snappy.parquet'))
