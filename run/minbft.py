import sys
import pathlib
import pyrem.task
import time

sys.path.append(pathlib.Path() / "run")
import common

common.setup("MinBFT performance")

duration = 10


def run(t, n_client):
    replica_task = [
        common.node[i + 1].run(
            common.replica_cmd(i, duration, "minbft", n_worker=14, batch_size=30),
            return_output=True,
        )
        for i in range(3)
    ]
    for i in range(3):
        replica_task[i].start()
    time.sleep(0.5)

    client_task = [
        common.node[5].run(common.client_cmd(i, duration, "minbft", t))
        for i in range(n_client)
    ]
    pyrem.task.Parallel(client_task).start(wait=True)

    for i in range(3):
        if not common.wait_replica(replica_task, i):
            return False
    return common.wait_client(client_task, t * n_client)


for t, n_client in [
    (4, 10),
    (6, 10),
    (8, 10),
    (16, 10),
    (32, 10),
    (64, 10),
    (96, 10),
]:
    while not run(t, n_client):
        pass
