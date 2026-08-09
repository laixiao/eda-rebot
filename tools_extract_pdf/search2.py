import sys

def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    path = sys.argv[1]
    kws = sys.argv[2:]
    text = open(path, encoding='utf-8').read()
    for i, line in enumerate(text.split('\n'), 1):
        for k in kws:
            if k in line:
                print(f'{i}: {line}')
                break

if __name__ == '__main__':
    main()