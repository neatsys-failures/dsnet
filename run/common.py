import re
import pathlib
import shutil
import sys
import pyrem.host
import pyrem.task


proj_dir = "/home/cowsay/dsnet/"
key_file = "/home/cowsay/keys.txt"
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

    targets = [
        "bench/client",
        "bench/replica",
        "nistore/benchClient",
        "nistore/replica",
        "timeserver/replica",
    ]

    node[0].run(
        [
            "rsync",
            "-a",
            "--exclude",
            ".obj",
            *[x for target in targets for x in ("--exclude", target)],
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
                *targets,
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
                    "-r",
                    proj_dir + "bench/",
                    f"nsl-node{node_index}.d1:{proj_dir}/bench",
                ]
            ),
            node[1].run(
                [
                    "rsync",
                    "-r",
                    proj_dir + "nistore/",
                    f"nsl-node{node_index}.d1:{proj_dir}/nistore",
                ]
            ),
            node[1].run(
                [
                    "rsync",
                    "-r",
                    proj_dir + "run/",
                    f"nsl-node{node_index}.d1:{proj_dir}/run",
                ]
            ),
        ]
    pyrem.task.Parallel(rsync_tasks, aggregate=True).start(wait=True)

    pyrem.task.Parallel(
        [node[i].run(["pkill", "-9", "replica"]) for i in (1, 2, 3, 4)]
    ).start(wait=True)
    print("!! Start !!")


def wait_replica(replica_task, i):
    replica_task[i].wait()
    output = replica_task[i].return_values["stderr"].decode()
    if re.search(f"Terminating on SIGTERM/SIGINT$", output, re.MULTILINE) is None:
        print(f"Replica {i} not finish normally")
        print(output)
        # sys.exit(1)
        return False
    with open(pathlib.Path() / "logs" / f"replica-{i}.txt", "w") as log_file:
        log_file.write(output)
    return True


def wait_client(client_task, prefix):
    assert all(task.host == client_task[0].host for task in client_task)
    rval = (
        pyrem.host.RemoteHost(client_task[0].host)
        .run(
            [
                "python3",
                proj_dir + "run/remote_report.py",
                str(len(client_task)),
                str(prefix),
            ],
            kill_remote=False,
        )
        .start(wait=True)
    )
    return rval["retcode"] == 0


config_file = "run/nsl0.config"


def replica_cmd(index, duration, mode, n_worker, batch_size=1):
    return [
        "timeout",
        f"{duration + 3}",
        proj_dir + "bench/replica",
        "-c",
        proj_dir + config_file,
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
        f"{duration + 2}",
        proj_dir + "bench/client",
        "-c",
        proj_dir + "run/nsl0.config",
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
