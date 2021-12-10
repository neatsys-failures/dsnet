import sys
import pathlib
sys.path.append(pathlib.Path() / 'run')
import common
import pyrem.task
import time

common.setup('MinBFT performance')

duration = 10
replica_task = [
    common.node[i + 1].run(
        common.replica_cmd(i, duration, "minbft", n_worker=4, batch_size=34),
        return_output=True,
    )
    for i in range(3)
]
for i in range(3):
    replica_task[i].start()
time.sleep(0.5)

client_task = [
    common.node[5].run(common.client_cmd(i, duration, "minbft", 8))
    for i in range(10)
]
pyrem.task.Parallel(client_task).start(wait=True)

for i in range(3):
    common.wait_replica(replica_task, i)
common.wait_client(client_task)
