# Bootstrap an environment close enough to NSL nodes, so the compiled result
# by this environment can be executed directly on NSL nodes

import subprocess as sp
import tempfile
import pathlib
import sys
import os

SYS_VERSION = 'Ubuntu 18.04.6 LTS'
if SYS_VERSION not in sp.run(['lsb_release', '-d'], stdout=sp.PIPE, encoding='utf8').stdout:
    print('Expect OS version: ', SYS_VERSION)
    sys.exit(1)

if 'PKG_CONFIG_PATH' not in os.environ:  # TODO
    with open(pathlib.Path.home() / '.bashrc', 'a+') as bashrc_file:
        bashrc_content = bashrc_file.read()
        bashrc_content += ('\n'
            'export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:/usr/local/lib64/pkgconfig"\n'
            'export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:/usr/local/lib:/usr/local/lib64:/usr/local/lib/x86_64-linux-gnu"\n'
        )
        bashrc_file.write(bashrc_content)
    print('Restart shell and continue bootstrap')
    sys.exit(0)

sp.run(['sudo', 'apt', 'update'])
apt_packages = [
    # universal
    'make',
    'clang',
    # dpdk
    'ninja-build',
    'python3-pip',
    'python3-pyelftools',
    # protobuf
    'autoconf', 
    'automake', 
    'libtool',
    'zlib1g-dev',
    # dsnet
    'pkg-config', 
    'libunwind-dev', 
    'libssl-dev', 
    'libevent-dev', 
    'libgtest-dev', 
    'libsecp256k1-dev',
]
sp.run(['sudo', 'apt', 'install', '-y', *apt_packages])
pip_packages = [
    # dpdk
    'meson==0.59.0',
    # dsnet (optional)
    'pyrem',
]
sp.run(['sudo', '-H', 'pip3', 'install', *pip_packages])

print('bootstrap: build DPDK')
with tempfile.TemporaryDirectory() as dpdk_workspace:
    sp.run(['curl', '-O', 'https://fast.dpdk.org/rel/dpdk-21.08.tar.xz'], cwd=dpdk_workspace)
    sp.run(['tar', '-xf', 'dpdk-21.08.tar.xz'], cwd=dpdk_workspace)
    sp.run(['meson', 'build'], cwd=pathlib.Path(dpdk_workspace) / 'dpdk-21.08')
    sp.run(['ninja'], cwd=pathlib.Path(dpdk_workspace) / 'dpdk-21.08' / 'build')
    sp.run(['sudo', 'ninja', 'install'], cwd=pathlib.Path(dpdk_workspace) / 'dpdk-21.08' / 'build')

print('bootstrap: build protobuf')
with tempfile.TemporaryDirectory() as protobuf_workspace:
    ASSET_URL = 'https://github.com/protocolbuffers/protobuf/releases/download/v3.17.3/protobuf-cpp-3.17.3.tar.gz'
    sp.run(['curl', '-LO', ASSET_URL], cwd=protobuf_workspace)
    sp.run(['tar', '-xf', 'protobuf-cpp-3.17.3.tar.gz'], cwd=protobuf_workspace)
    sp.run(['./configure'], cwd=pathlib.Path(protobuf_workspace) / 'protobuf-3.17.3')
    sp.run(['make'], cwd=pathlib.Path(protobuf_workspace) / 'protobuf-3.17.3')
    sp.run(['sudo', 'make', 'install'], cwd=pathlib.Path(protobuf_workspace) / 'protobuf-3.17.3')

print('bootstrap: test setup (run unrelicated test)')
sp.run(['make', 'run-tests/replication/unreplicated-test'], cwd=pathlib.Path(), check=True)
print('bootstrap: test setup (run unreplicated benchmark)')
sp.run(['make', 'bench/replica', 'bench/client'], cwd=pathlib.Path(), check=True)
replica_proc = sp.Popen([
    'bench/replica', 
    '-c', 'run/localhost.txt',
    '-m', 'unreplicated',
    '-i', '0',
], cwd=pathlib.Path())
sp.run([
    'bench/client', 
    '-c', 'run/localhost.txt',
    '-m', 'unreplicated',
    '-h', 'localhost',
], cwd=pathlib.Path(), check=True)
replica_proc.kill()
