import sys
import pathlib
import pyrem.task
import time

sys.path.append(pathlib.Path() / "run")
import common

common.config_file = "run/nsl-large.config"

common.setup("TOMBFT performance")

duration = 1
n = 12


def run(t, n_client):
    replica_task = [
        common.node[i % 4 + 1].run(
            common.replica_cmd(i, duration + 1, "tombft", n_worker=4),
            return_output=True,
        )
        for i in range(n)
    ]
    for i in range(n):
        replica_task[i].start()

    time.sleep(1)
    client_task = [
        common.node[5].run(common.client_cmd(i, duration, "tombft", t))
        for i in range(n_client)
    ]
    pyrem.task.Parallel(client_task).start(wait=True)

    replica_alive = True
    for i in range(n):
        if not common.wait_replica(replica_task, i):
            replica_alive = False
    if not replica_alive:
        return False
    return common.wait_client(client_task, t * n_client)


run(1, 10)
