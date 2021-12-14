import sys
import pathlib
import pyrem.task
import time

sys.path.append(pathlib.Path() / "run")
import common

common.setup("TOMBFT performance")

duration = 1


def run(t, n_client):
    replica_task = [
        common.node[i + 1].run(
            common.replica_cmd(i, duration, "tombft", n_worker=4), return_output=True
        )
        for i in range(4)
    ]
    for i in range(4):
        replica_task[i].start()

    time.sleep(0.5)
    client_task = [
        common.node[5].run(common.client_cmd(i, duration, "tombft", t))
        for i in range(n_client)
    ]
    pyrem.task.Parallel(client_task).start(wait=True)

    replica_alive = True
    for i in range(4):
        if not common.wait_replica(replica_task, i):
            replica_alive = False
    if not replica_alive:
        return False
    return common.wait_client(client_task, t * n_client)


for t, n_client in [
    # (1, 1),
    # (1, 2),
    # (1, 5),
    # (1, 10),
    # (2, 10),
    # (5, 5),
    # (3, 10),
    # (4, 10),
    # (5, 10),
    # (6, 10),
    # (8, 10),
    # (10, 10),
    # (12, 10),
    # (14, 10),
    # (16, 10),
    # (18, 10),
    # (20, 10),
    # (25, 10),
    # (30, 10),
    (35, 10),
    # (40, 10),
    # (42, 10),
    # (44, 10),
    # (46, 10),
    # (50, 10),
    # (55, 10),
    # (60, 10),
    # (80, 10),
    # (85, 10),
    # (90, 10),
]:
    while not run(t, n_client):
        pass
