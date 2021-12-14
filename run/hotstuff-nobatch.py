import sys
import pathlib

sys.path.append(pathlib.Path() / "run")
import common
import pyrem.task
import time

common.setup("HotStuff performance")

duration = 10


def run(t, n_client):
    replica_task = [
        common.node[i + 1].run(
            common.replica_cmd(i, duration + 1, "hotstuff", n_worker=14),
            return_output=True,
        )
        for i in range(4)
    ]
    # start backup before primary to prevent backup missing first QC
    for i in (1, 2, 3, 0):
        replica_task[i].start()

    client_task = [
        common.node[5].run(common.client_cmd(i, duration, "hotstuff", t))
        for i in range(n_client)
    ]
    time.sleep(0.8)
    pyrem.task.Parallel(client_task).start(wait=True)

    for i in range(4):
        if not common.wait_replica(replica_task, i):
            return False
    return common.wait_client(client_task, t * n_client)


for t, n_client in [
    (1, 10),
    (2, 10),
    (4, 10),
    (5, 10),
    (6, 10),
    (7, 10),
    (8, 10),
    (16, 10),
    (20, 10),
    # (24, 10),
    # (28, 10),
    # (32, 10),
]:
    while not run(t, n_client):
        pass
