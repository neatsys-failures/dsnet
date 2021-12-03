import sys
import pathlib
sys.path.append(pathlib.Path() / 'run')
import common
import pyrem.task
import time

common.setup('PBFT performance')

duration = 10
def replica_cmd(index):
    return [
        'timeout', f'{duration + 3}',
        common.proj_dir + 'bench/replica',
        '-c', common.proj_dir + 'run/nsl.txt',
        '-m', 'pbft',
        '-i', f'{index}',
        '-w', '4',
    ]
client_cmd = [
    'timeout', f'{duration + 3}',
    common.proj_dir  + 'bench/client',
    '-c', common.proj_dir + 'run/nsl.txt',
    '-m', 'pbft',
    '-h', '11.0.0.101',
    '-u', f'{duration}',
    '-t', '8',
]

replica_task = [None] * 4
for i in range(4):
    replica_task[i] = common.node[i + 1].run(replica_cmd(i), return_output=True)
    replica_task[i].start()
time.sleep(0.1)
client_task = [
    common.node[5].run(client_cmd, return_output=True)
    for _ in range(1)
]
pyrem.task.Parallel(client_task).start(wait=True)

for i in range(4):
    common.wait_replica(replica_task, i)
common.wait_client(client_task)
