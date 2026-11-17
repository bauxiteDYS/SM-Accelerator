import sys
from ambuild2 import run

parser = run.BuildParser(sourcePath = sys.path[0], api='2.2')
parser.options.add_argument('--sm-path', type=str, dest='sm_path', default=None,
                        help='Path to SourceMod')
parser.options.add_argument('--sm-version', choices=['1.11', '1.12', '1.13', 'custom'], default='custom',
                        help='Validate the SourceMod source target (default: custom)')
parser.options.add_argument('--enable-debug', action='store_const', const='1', dest='debug',
                        help='Enable debugging symbols')
parser.options.add_argument('--enable-optimize', action='store_const', const='1', dest='opt',
                        help='Enable optimization')
parser.options.add_argument('--targets', type=str, dest='targets', default=None,
		                help="Override the target architecture (use commas to separate multiple targets).")
parser.Configure()
