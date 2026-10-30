import sys

from .runner import main

# Piped, Python writes cp1252, and a character it lacks would end the run at the line that holds it.
sys.stdout.reconfigure(errors="replace")
sys.exit(main(sys.argv[1:]))
