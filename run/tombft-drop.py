import sys
import pathlib

sys.path.append(pathlib.Path() / "run")
import common
import pyrem.task
import time

common.setup("TOMBFT performance")

duration = 10


def run(t, n_client):
    replica_task = [
        common.node[i + 1].run(
            common.replica_cmd(i, duration, "tombft", n_worker=4)
            + ["-d", "0.005"],
            return_output=True,
        )
        for i in range(4)
    ]
    for i in range(4):
        replica_task[i].start()
    time.sleep(0.3)

    client_task = [
        common.node[5].run(common.client_cmd(i, duration, "tombft", t))
        for i in range(n_client)
    ]
    pyrem.task.Parallel(client_task).start(wait=True)

    alive = True
    for i in range(4):
        if not common.wait_replica(replica_task, i):
            alive = False
    if not alive:
        return False
    return common.wait_client(client_task, t * n_client)


run(14, 20)
