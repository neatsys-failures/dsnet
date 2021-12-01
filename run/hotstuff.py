import sys
import pathlib
sys.path.append(pathlib.Path() / 'run')
import common
import pyrem.task
import time

common.setup('HotStuff performance')

duration = 3
def replica_cmd(index):
    return [
        'timeout', f'{duration + 3}',
        common.proj_dir + 'bench/replica',
        '-c', common.proj_dir + 'run/nsl.txt',
        '-m', 'hotstuff',
        '-i', f'{index}',
        '-w', '12',
        '-b', '20',
    ]
client_cmd = [
    'timeout', f'{duration + 3}',
    common.proj_dir  + 'bench/client',
    '-c', common.proj_dir + 'run/nsl.txt',
    '-m', 'hotstuff',
    '-h', '11.0.0.101',
    '-u', f'{duration}',
    '-t', '20',
]

replica_task = [None] * 4
for i in (1, 2, 3, 0):
    replica_task[i] = common.node[i + 1].run(replica_cmd(i), return_output=True)
    replica_task[i].start()
time.sleep(0.1)
client_task = [
    common.node[5].run(client_cmd, return_output=True)
    for _ in range(12)
]
pyrem.task.Parallel(client_task).start(wait=True)

for i in range(4):
    common.wait_replica(replica_task, i)
common.wait_client(client_task)
