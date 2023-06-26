import pandas as pd
import matplotlib.pyplot as plt

file_name = "data.txt"

# read data from file
data = pd.read_csv(file_name, header=None)

# plot data
plt.figure(figsize=(10, 6))
plt.plot(data[0])
plt.title("Plot from Text File Data")
plt.xlabel("Line Number")
plt.ylabel("Value")
plt.grid(True)
plt.show()
