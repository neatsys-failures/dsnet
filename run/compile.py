import re
import pathlib
import sys
import pyrem.host
import pyrem.task

print('Compile')
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

for node_index in (2, 3, 4, 5):
    node[1].run([
        'rsync', proj_dir + 'bench/client', f'nsl-node{node_index}.d1:{proj_dir}' + 'bench/client', 
    ]).start(wait=True)
    node[1].run([
        'rsync', proj_dir + 'bench/replica', f'nsl-node{node_index}.d1:{proj_dir}' + 'bench/replica', 
    ]).start(wait=True)
    node[1].run([
        'rsync', proj_dir + 'run/nsl.txt', f'nsl-node{node_index}.d1:{proj_dir}' + 'run/nsl.txt', 
    ]).start(wait=True)

