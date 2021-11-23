import re
import pathlib
import pyrem.host
import pyrem.task

print('Standard NSL system performance check')
print('Expect ~360000 <=42')
proj_dir = '/home/cowsay/dsnet/'
local_dir = '/work/dsnet/'
for i in (1, 5):
    pyrem.host.LocalHost().run([
        'rsync', '-a', local_dir, f'nsl-node{i}:' + proj_dir[:-1]
    ]).start(wait=True)

replica_cmd = [
    'taskset', '0x01',
    proj_dir + 'bench/replica',
    '-c', proj_dir + 'run/nsl.txt',
    '-m', 'unreplicated',
    '-i', '0',
]
client_cmd = [
    'timeout', '12',
    proj_dir  + 'bench/client',
    '-c', proj_dir + 'run/nsl.txt',
    '-m', 'unreplicated',
    '-h', '11.0.0.101',
    '-u', '10',
]
node = [
    pyrem.host.RemoteHost('nsl-node1'),
    pyrem.host.RemoteHost('nsl-node2'),
    pyrem.host.RemoteHost('nsl-node3'),
    pyrem.host.RemoteHost('nsl-node4'),
    pyrem.host.RemoteHost('nsl-node5'),
]
node[0].run(replica_cmd, quiet=True).start()
client_task = [
    node[4].run(client_cmd  # + ['-s', proj_dir + f'stat-{i}', '-i', '1']
    , return_output=True)
    for i in range(14)
]
pyrem.task.Parallel(client_task).start(wait=True)
throughput_sum = 0
median_latency_max = 0
for i, task in enumerate(client_task):
    output = task.return_values['stderr'].decode()
    match = re.search(r'Total throughput is (\d+) ops/sec$', output, re.MULTILINE)
    if match is not None:
        throughput_sum += int(match[1])
    else:
        print(f'warning: no data from client-{i}')
        with open(pathlib.Path() / 'logs' / f'client-{i}.txt', 'w') as log_file:
            log_file.write(output)
        continue
    match = re.search(r'Median latency is (\d+) us$', output, re.MULTILINE)
    median_latency_max = max(median_latency_max, int(match[1]))
print(f'        {throughput_sum}   {median_latency_max}')
