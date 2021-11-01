import re
import pathlib
import pyrem.host
import pyrem.task

print('baseline')
proj_dir = '/home/cowsay/dsnet/'
local_dir = '/ws/dsnet/'
for i in range(5):
    pyrem.host.LocalHost().run([
        'rsync', '-a', 
        local_dir, f'cowsay@nsl-node{i + 1}.d1.comp.nus.edu.sg:' + proj_dir[:-1]
    ]).start(wait=True)

replica_cmd = [
    proj_dir + 'bench/replica',
    '-c', proj_dir + 'run/nsl.txt',
    '-m', 'unreplicated',
    '-i', '0',
]
client_cmd = [
    proj_dir  + 'bench/client',
    '-c', proj_dir + 'run/nsl.txt',
    '-m', 'unreplicated',
    '-h', '11.0.0.101',
    '-u', '30',
]
node = [
    pyrem.host.RemoteHost('cowsay@nsl-node1.d1.comp.nus.edu.sg'),
    pyrem.host.RemoteHost('cowsay@nsl-node2.d1.comp.nus.edu.sg'),
    pyrem.host.RemoteHost('cowsay@nsl-node3.d1.comp.nus.edu.sg'),
    pyrem.host.RemoteHost('cowsay@nsl-node4.d1.comp.nus.edu.sg'),
    pyrem.host.RemoteHost('cowsay@nsl-node5.d1.comp.nus.edu.sg'),
]
node[0].run(replica_cmd).start()
client_task = [
    node[4].run(client_cmd, return_output=True)
    for _ in range(24)
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
print(throughput_sum, median_latency_max)
