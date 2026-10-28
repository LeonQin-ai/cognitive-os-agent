"""Run the reciprocal discussion lifecycle against native complete_task responses."""
import os
from pathlib import Path
import runpy

os.environ['COA_IM_TEST_PROTOCOL']='native'
runpy.run_path(str(Path(__file__).with_name('test_im_discussion_e2e.py')),run_name='__main__')
