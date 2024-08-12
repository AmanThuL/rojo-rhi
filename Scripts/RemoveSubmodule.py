import subprocess


def list_submodules():
    try:
        # Get the list of submodules
        output = subprocess.check_output(['git', 'submodule', 'status'], universal_newlines=True)
        submodules = output.strip().split('\n')

        # Extract the submodule names
        submodule_names = [submodule.split()[1] for submodule in submodules if len(submodule.split()) > 1]

        return submodule_names
    except subprocess.CalledProcessError as e:
        print(f"Error while listing submodules: {e}")
        return []


def remove_submodule(submodule_name):
    try:
        # Remove the specified submodule
        subprocess.run(['git', 'submodule', 'deinit', '-f', submodule_name], check=True)
        subprocess.run(['git', 'rm', '-f', submodule_name], check=True)
        subprocess.run(['rm', '-rf', '.git/modules/' + submodule_name], check=True)

        print(f'Submodule "{submodule_name}" has been removed.')
    except subprocess.CalledProcessError as e:
        print(f"Error while removing submodule: {e}")


def main():
    submodules = list_submodules()

    if not submodules:
        print('No submodules found in this repository.')
        return

    print('Submodules in this repository:')
    for i, submodule in enumerate(submodules):
        print(f'{i+1}. {submodule}')

    choice = input('Enter the index of the submodule to remove (or "q" to quit): ')

    if choice == 'q':
        return

    try:
        choice_index = int(choice) - 1
        if 0 <= choice_index < len(submodules):
            remove_submodule(submodules[choice_index])
        else:
            print('Invalid choice.')
    except ValueError:
        print('Invalid input. Please enter a number.')

if __name__ == "__main__":
    main()