import sys
import pathlib

sys.path.append(pathlib.Path() / "run")
import common
import pyrem.task

common.setup("Standard NSL system performance check")
print("Expect ~360000 <=42 (recently dropped a little)")

duration = 10
replica_task = common.node[1].run(
    ["taskset", "0x1"] + common.replica_cmd(0, duration, "unreplicated", 1),
    return_output=True,
)
replica_task.start()

client_task = [
    common.node[5].run(
        common.client_cmd(i, duration, "unreplicated", 3), return_output=True
    )
    for i in range(6)
]
pyrem.task.Parallel(client_task).start(wait=True)

common.wait_replica([replica_task], 0)
common.wait_client(client_task)
