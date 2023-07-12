import matplotlib.pyplot as plt


scores = []
idx = 0

with open("scores.txt") as f:
    s = f.read()
    for line in s.split("\n"):
        if line != "":
            scores.append((idx, int(line)))
            idx += 1

data = sorted(scores, key=lambda x: x[1])
# print(scores)


# plot data
plt.figure(figsize=(10, 6))
plt.plot(data)
plt.title("Plot from Text File Data")
plt.xlabel("Line Number")
plt.ylabel("Value")
plt.grid(True)
plt.show()
