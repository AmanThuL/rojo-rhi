import os

# Change directory to the script's location
script_dir = os.path.dirname(os.path.abspath(__file__))
os.chdir(script_dir)
os.chdir("../")

# Run git submodule update --init --recursive
os.system("git submodule update --init --recursive")

# Create Build directory
os.makedirs("Build", exist_ok=True)

# Change directory to Build
os.chdir("Build")

# Run cmake
#os.system("cmake ..")