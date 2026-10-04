import os,sys,tempfile,subprocess,threading,hmac,hashlib,json
from pathlib import Path
# Run with the MBIIEZ API virtualenv Python. No production instance is started.
import argparse,shutil
parser=argparse.ArgumentParser();parser.add_argument('--mbiiez',type=Path,default=Path(__file__).resolve().parents[3]/'mbiiez');args=parser.parse_args()
sys.path.insert(0,str(args.mbiiez))
from werkzeug.serving import make_server
from mbiiez import settings,bansync
from mbiiez.api import shared_node as node,shared_ledger as ledger
from mbiiez.api.server import create_app
from mbiiez.api.storage import write,read
with tempfile.TemporaryDirectory(prefix='native-shared-proof-') as directory:
    root=Path(directory);data=root/'MBII';data.mkdir()
    here=Path(__file__).resolve().parent;source=here.parents[1]/'codemp/server'
    for filename in ('shared_ledger.cpp','shared_ledger.h','cJSON.h','cJSON.c'):shutil.copyfile(source/filename,root/filename)
    for filename in ('server.h','main.cpp'):shutil.copyfile(here/filename,root/filename)
    subprocess.run(['g++','-std=c++11','-O2',str(root/'shared_ledger.cpp'),str(root/'cJSON.c'),str(root/'main.cpp'),'-o',str(root/'check')],check=True)
    os.environ['MBIIEZ_STATE_DIR']=str(root/'state');settings.locations.mbii_path=str(data)
    salt=b'a'*16;pin=hmac.new(salt,b'1234',hashlib.md5).hexdigest()
    (data/'economy_accounts.dat').write_text(f'Player {salt.hex()} {pin} 100 0 0\n')
    node.safety=lambda:{'ready':True,'shared_protocol':2,'note':''};bansync.sync=lambda:[]
    server=make_server('127.0.0.1',0,create_app());os.environ['MBIIEZ_API_PORT']=str(server.server_port)
    node.activate('local',authority=True)
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    try:
        result=subprocess.run([str(root/'check')],check=True,capture_output=True,text=True)
        print(result.stdout.strip());node.exchange();node.exchange()
        with ledger.transaction() as db:
            assert ledger.get(db,'accounts','player')[3]==90
            assert ledger.get(db,'stats','h:player')[1]==1
            assert ledger.get(db,'stats','h:other')[2]==1
        print('Actual C++ HTTP/JSON bridge + authenticated Python agent + SQLite authority: confirmed final balance 90 and counters exactly once.')
    finally:server.shutdown()
