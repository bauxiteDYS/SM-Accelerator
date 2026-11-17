# vim: set ts=2 sw=2 tw=99 noet ft=python:
import subprocess, sys
from pathlib import Path

argv = sys.argv[3:]
cwd = sys.argv[1]
print('CMD: ' + ' '.join(argv))
print('CWD: ' + cwd)
print('OUT: ' + sys.argv[2])
# The patch stage uses literal && separators. Execute each command directly so
# paths containing spaces remain intact, on both Windows and Linux.
commands = [[]]
for arg in argv:
  if arg == '&&':
    commands.append([])
  else:
    commands[-1].append(arg)
for command in commands:
  result = subprocess.run(command, cwd=cwd)
  if result.returncode != 0:
    sys.exit(result.returncode)

# Do not leave a successful-build stamp behind if a command failed.
if sys.argv[2] != '_':
  Path(sys.argv[2]).touch()
