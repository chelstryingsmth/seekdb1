"""Run portable file-reader checks when the complete seekdb build is unavailable."""
import argparse
import base64
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default='g++')
    parser.add_argument('--rapidjson-include', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[4]
    include = args.rapidjson_include
    if include is None:
        for candidate in [root/'deps/3rd/vcpkg/x64-windows/include',
                          root/'deps/3rd/usr/local/oceanbase/deps/devel/include']:
            if (candidate/'rapidjson/document.h').exists():
                include = candidate
                break
    environment = dict(os.environ)
    compiler = Path(args.compiler)
    if compiler.is_absolute():
        environment['PATH'] = str(compiler.parent)+os.pathsep+environment.get('PATH','')
    def run(command):
        print('COMMAND:', shlex.join(map(str,command)),flush=True)
        result = subprocess.run(list(map(str,command)),env=environment,cwd=root)
        print('EXIT:',result.returncode,flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)
    with tempfile.TemporaryDirectory(prefix='seekdb-reader-tests-') as scratch:
        scratch = Path(scratch)
        for name in ['test_csv_reader_standalone','test_jsonl_reader','test_secure_file_input',
                     'test_csv_output','test_parquet_reader']:
            executable = scratch/(name+('.exe' if os.name == 'nt' else ''))
            command = [args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-DNDEBUG',
                       '-DFILE_SQL_STANDALONE_TEST','-I'+str(root/'src')]
            if include:
                command += ['-isystem',str(include)]
            command += [root/'unittest/sql/engine/basic'/(name+'.cpp'),'-o',executable]
            run(command)
            if name == 'test_parquet_reader':
                public = scratch/'public.parquet'
                public.write_bytes(base64.b64decode((root/'tools/deploy/mysql_test/test_suite/file_sql/data/simple.parquet.b64').read_bytes()))
                run([executable,public])
                run([executable,root/'unittest/sql/engine/basic/fixtures/mixed_snappy.parquet','mixed'])
            else:
                run([executable])
    print('All portable file-reader checks passed. These checks do not verify native SQL integration.',flush=True)

if __name__ == '__main__':
    main()
