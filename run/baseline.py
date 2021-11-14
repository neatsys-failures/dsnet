import re
import pathlib
import sys
import pyrem.host
import pyrem.task

print('Baseline performance')
for line in open(pathlib.Path() / 'Makefile'):
    if '-O3' in line and line.strip().startswith('#'):
        print('Makefile is not in benchmark mode')
        sys.exit(1)

proj_dir = '/home/cowsay/dsnet/'
local_dir = '/ws/dsnet/'
duration = 10

node = [
    pyrem.host.LocalHost(),
    pyrem.host.RemoteHost('nsl-node1'),
    pyrem.host.RemoteHost('nsl-node2'),
    pyrem.host.RemoteHost('nsl-node3'),
    pyrem.host.RemoteHost('nsl-node4'),
    pyrem.host.RemoteHost('nsl-node5'),
]
node[0].run([
    'rsync', '-a', 
    '--exclude', '.obj', 
    '--exclude', 'bench/client',
    '--exclude', 'bench/replica',
    local_dir, f'nsl-node1:' + proj_dir[:-1]
]).start(wait=True)
rval = node[1].run(['make', '-j', '64', '-C', '/home/cowsay/dsnet', 'bench/client', 'bench/replica']).start(wait=True)
if rval['retcode'] != 0:
    sys.exit(1)
node[1].run([
    'rsync', proj_dir + 'bench/client', f'nsl-node5.d1:{proj_dir}' + 'bench/client', 
]).start(wait=True)


replica_cmd = [
    'timeout', f'{duration + 2}',
    proj_dir + 'bench/replica',
    '-c', proj_dir + 'run/nsl.txt',
    '-m', 'signedunrep',
    '-i', '0',
    '-w', '15',
]
client_cmd = [
    'timeout', f'{duration + 2}',
    proj_dir  + 'bench/client',
    '-c', proj_dir + 'run/nsl.txt',
    '-m', 'signedunrep',
    '-h', '11.0.0.101',
    '-u', f'{duration}',
    '-t', '4',
]

replica_task = node[1].run(replica_cmd, kill_remote=False)
replica_task.start()
client_task = [
    node[5].run(client_cmd, return_output=True)
    for _ in range(16)
]
pyrem.task.Parallel(client_task).start(wait=True)
replica_task.wait()
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
print(throughput_sum, median_latency_max)
