import sys
import pathlib
import re

sys.path.append(pathlib.Path() / "run")
import common

throughput_sum = 0
median_latency_max = 0
for i in range(int(sys.argv[1])):
    output = pathlib.Path(common.proj_dir + f"client-{i}.txt").read_text()
    match = re.search(r"Total throughput is (\d+) ops/sec$", output, re.MULTILINE)
    if match is not None:
        throughput_sum += int(match[1])
    else:
        print(f"warning: no data from client-{i}")
        # with open(pathlib.Path() / "logs" / f"client-{i}.txt", "w") as log_file:
        #     log_file.write(output)
        continue
    match = re.search(r"Median latency is (\d+) us$", output, re.MULTILINE)
    median_latency_max = max(median_latency_max, int(match[1]))
print(throughput_sum, median_latency_max)
