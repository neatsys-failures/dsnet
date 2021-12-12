import sys
import pathlib

sys.path.append(pathlib.Path() / "run")
import common
import pyrem.task
import time

common.setup("TOMBFT-HMAC performance")

duration = 10


def run(t, n_client):
    replica_task = [
        common.node[i + 1].run(
            common.replica_cmd(i, duration, "tombft-hmac", n_worker=4),
            return_output=True,
        )
        for i in range(4)
    ]
    for i in range(4):
        replica_task[i].start()
    time.sleep(0.3)

    client_task = [
        common.node[5].run(common.client_cmd(i, duration, "tombft-hmac", t))
        for i in range(n_client)
    ]
    pyrem.task.Parallel(client_task).start(wait=True)

    for i in range(4):
        if not common.wait_replica(replica_task, i):
            return False
    return common.wait_client(client_task, t * n_client)


for t, n_client in [
    (1, 1),
    (1, 3),
    (1, 6),
    (1, 10),
    (1, 15),
    (2, 10),
    (2, 15),
    (4, 10),
]:
    while not run(t, n_client):
        pass
