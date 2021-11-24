import sys
import pathlib
sys.path.append(pathlib.Path() / 'run')
import common
import pyrem.task

common.setup('TOMBFT performance')

duration = 20
def replica_cmd(index):
    return [
        'timeout', f'{duration + 3}',
        proj_dir + 'bench/replica',
        '-c', proj_dir + 'run/nsl.txt',
        '-m', 'tombft',
        '-i', f'{index}',
        '-w', '24',
    ]
client_cmd = [
    'timeout', f'{duration + 3}',
    proj_dir  + 'bench/client',
    '-c', proj_dir + 'run/nsl.txt',
    '-m', 'tombft',
    '-h', '11.0.0.101',
    '-u', f'{duration}',
    '-t', '5',
]

replica_task = [None] * 4
for i in range(4):
    # if i == 1: continue
    replica_task[i] = node[i + 1].run(replica_cmd(i), kill_remote=False, return_output=True)
    replica_task[i].start()
client_task = [
    node[5].run(client_cmd, return_output=True)
    for _ in range(20)
]
pyrem.task.Parallel(client_task).start(wait=True)

for i in range(4):
    common.wait_replica(i)
common.wait_client()
