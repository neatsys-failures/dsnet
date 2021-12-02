import sys
import pathlib
sys.path.append(pathlib.Path() / 'run')
import common
import pyrem.task

common.setup('Baseline performance')

duration = 10
def replica_cmd(index):
    return [
        'timeout', f'{duration + 3}',
        common.proj_dir + 'bench/replica',
        '-c', common.proj_dir + 'run/nsl.txt',
        '-m', 'signedunrep',
        '-i', f'{index}',
        '-w', '3',
    ]
client_cmd = [
    'timeout', f'{duration + 3}',
    common.proj_dir  + 'bench/client',
    '-c', common.proj_dir + 'run/nsl.txt',
    '-m', 'signedunrep',
    '-h', '11.0.0.101',
    '-u', f'{duration}',
    '-t', '4',
]

replica_task = common.node[1].run(replica_cmd(0), return_output=True)
replica_task.start()
client_task = [
    common.node[5].run(client_cmd, return_output=True)
    for _ in range(14)
]
pyrem.task.Parallel(client_task).start(wait=True)

common.wait_replica([replica_task], 0)
common.wait_client(client_task)
