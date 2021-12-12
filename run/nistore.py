import sys
import pathlib
import pyrem.task
import time

sys.path.append(pathlib.Path() / "run")
import common

common.setup("nistore performance")

mode = "tombft-hmac"

for i in range(3):
    common.node[1].run(
        [
            common.proj_dir + "timeserver/replica",
            "-c",
            common.proj_dir + "run/nsl.tss.config",
            "-m",
            "vr",
            "-i",
            str(i),
        ]
    ).start()
time.sleep(1)

for i in range(4):
    common.node[i + 1].run(
        [
            common.proj_dir + "nistore/replica",
            "-c",
            common.proj_dir + "run/nsl0.config",
            "-m",
            mode,
            "-i",
            str(i),
        ]
    ).start()
time.sleep(1)

pyrem.task.Parallel(
    [
        common.node[5].run(
            [
                common.proj_dir + "nistore/benchClient",
                "-c",
                common.proj_dir + "run/nsl",
                "-m",
                mode,
                "-f",
                common.key_file,
                "-h",
                "11.0.0.101",
            ]
        )
        for i in range(1)
    ]
).start(wait=True)
