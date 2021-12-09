import sys
import pathlib

sys.path.append(pathlib.Path() / "run")
import common
import pyrem.task

common.setup("Baseline performance")

duration = 10

replica_task = common.node[1].run(
    common.replica_cmd(0, duration, "signedunrep", 4, 1), return_output=True
)
replica_task.start()
client_task = [
    common.node[5].run(
        common.client_cmd(i, duration, "signedunrep", 20), return_output=True
    )
    for i in range(8)
]
pyrem.task.Parallel(client_task).start(wait=True)

common.wait_replica([replica_task], 0)
common.wait_client(client_task)
