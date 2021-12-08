import re
import pathlib
import shutil
import sys
import pyrem.host
import pyrem.task


proj_dir = "/home/cowsay/dsnet/"
local_dir = "/work/dsnet/"
log_dir = pathlib.Path() / "logs"

node = [
    pyrem.host.LocalHost(),
    pyrem.host.RemoteHost("nsl-node1"),
    pyrem.host.RemoteHost("nsl-node2"),
    pyrem.host.RemoteHost("nsl-node3"),
    pyrem.host.RemoteHost("nsl-node4"),
    pyrem.host.RemoteHost("nsl-node5"),
]


def setup(title):
    print(title)
    for line in open(pathlib.Path() / "Makefile"):
        if "-O3" in line and line.strip().startswith("#"):
            print("Makefile is not in benchmark mode")
            sys.exit(1)

    shutil.rmtree(log_dir, ignore_errors=True)
    log_dir.mkdir()
    (log_dir / ".gitignore").write_text("*")

    node[0].run(
        [
            "rsync",
            "-a",
            "--exclude",
            ".obj",
            "--exclude",
            "bench/client",
            "--exclude",
            "bench/replica",
            local_dir,
            f"nsl-node1:" + proj_dir[:-1],
        ]
    ).start(wait=True)
    # TODO abort on compile fail
    rval = (
        node[1]
        .run(
            [
                "make",
                "-j",
                "64",
                "-C",
                "/home/cowsay/dsnet",
                "bench/client",
                "bench/replica",
            ],
            kill_remote=False,
        )
        .start(wait=True)
    )
    if rval["retcode"] != 0:
        sys.exit(1)

    rsync_tasks = []
    for node_index in (2, 3, 4, 5):
        rsync_tasks += [
            node[1].run(
                [
                    "rsync",
                    proj_dir + "bench/client",
                    f"nsl-node{node_index}.d1:{proj_dir}/bench/client",
                ]
            ),
            node[1].run(
                [
                    "rsync",
                    proj_dir + "bench/replica",
                    f"nsl-node{node_index}.d1:{proj_dir}/bench/replica",
                ]
            ),
            node[1].run(
                [
                    "rsync",
                    proj_dir + "run/nsl.txt",
                    f"nsl-node{node_index}.d1:{proj_dir}/run/nsl.txt",
                ]
            ),
        ]
    for task in rsync_tasks:
        task.start()
    for task in rsync_tasks:
        task.wait()

    print("Start")


def wait_replica(replica_task, i):
    replica_task[i].wait()
    output = replica_task[i].return_values["stderr"].decode()
    if re.search(f"Terminating on SIGTERM/SIGINT$", output, re.MULTILINE) is None:
        print(f"Replica {i} not finish normally")
        print(output)
        sys.exit(1)
    with open(pathlib.Path() / "logs" / f"replica-{i}.txt", "w") as log_file:
        log_file.write(output)


def wait_client(client_task):
    throughput_sum = 0
    median_latency_max = 0
    for i, task in enumerate(client_task):
        output = (
            pyrem.host.RemoteHost(task.host)
            .run(
                ["cat", proj_dir + f"client-{i}.txt"],
                return_output=True,
            )
            .start(wait=True)["stdout"]
            .decode()
        )
        match = re.search(r"Total throughput is (\d+) ops/sec$", output, re.MULTILINE)
        if match is not None:
            throughput_sum += int(match[1])
        else:
            print(f"warning: no data from client-{i}")
            with open(pathlib.Path() / "logs" / f"client-{i}.txt", "w") as log_file:
                log_file.write(output)
            continue
        match = re.search(r"Median latency is (\d+) us$", output, re.MULTILINE)
        median_latency_max = max(median_latency_max, int(match[1]))
    print(throughput_sum, median_latency_max)


def replica_cmd(index, duration, mode, n_worker, batch_size=1):
    return [
        "timeout",
        f"{duration + 2}",
        proj_dir + "bench/replica",
        "-c",
        proj_dir + "run/nsl.txt",
        "-m",
        mode,
        "-i",
        f"{index}",
        "-w",
        f"{n_worker}",
        "-b",
        f"{batch_size}",
    ]


def client_cmd(index, duration, mode, n_thread):
    return [
        "timeout",
        f"{duration + 1}",
        proj_dir + "bench/client",
        "-c",
        proj_dir + "run/nsl.txt",
        "-m",
        mode,
        "-h",
        "11.0.0.101",
        "-u",
        f"{duration}",
        "-t",
        f"{n_thread}",
        "2>",
        proj_dir + f"client-{index}.txt",
    ]
