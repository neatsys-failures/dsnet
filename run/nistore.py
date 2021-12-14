import sys
import pathlib
import pyrem.task
import time
import re

sys.path.append(pathlib.Path() / "run")
import common

common.setup("nistore performance")

mode = "tombft"

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

client_task = [
    common.node[5].run(
        [
            common.proj_dir + "nistore/benchClient",
            "-c",
            common.proj_dir + "run/nsl",
            "-m",
            mode,
            "-h",
            "11.0.0.101",
            "-f",
            common.key_file,
            "-k",
            "10000",
            "-w",
            "10",
            ">",
            f"client-{i}.txt",
            "2>",
            "/dev/null",
        ],
    )
    for i in range(240)
]
pyrem.task.Parallel(client_task, aggregate=True).start(wait=True)
throughput_sum = 0
max_latency = 0
for i, task in enumerate(client_task):
    print(".", end="", flush=True)
    output = (
        common.node[5]
        .run(["cat", f"client-{i}.txt"], return_output=True)
        .start(wait=True)["stdout"]
        .decode()
    )
    # print(output)
    match = re.search(r"Commit: (\d+)", output, re.MULTILINE)
    if match is not None:
        throughput_sum += int(match[1])
    else:
        print(f"warning: no data from client-{i}")
        continue
    match = re.search(r"Overall_Latency: (\d+\.\d+)", output, re.MULTILINE)
    max_latency = max(max_latency, float(match[1]))
print()
print(throughput_sum / 10, max_latency)
