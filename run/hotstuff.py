import sys
import pathlib

sys.path.append(pathlib.Path() / "run")
import common
import pyrem.task
import time

common.setup("HotStuff performance")

duration = 10
replica_task = [
    common.node[i + 1].run(
        common.replica_cmd(i, duration + 1, "hotstuff", n_worker=14, batch_size=38),
        return_output=True,
    )
    for i in range(4)
]
# start backup before primary to prevent backup missing first QC
for i in (1, 2, 3, 0):
    replica_task[i].start()

client_task = [
    common.node[5].run(common.client_cmd(i, duration, "hotstuff", 60))
    for i in range(10)
]
time.sleep(0.8)
pyrem.task.Parallel(client_task).start(wait=True)

for i in range(4):
    common.wait_replica(replica_task, i)
common.wait_client(client_task)
