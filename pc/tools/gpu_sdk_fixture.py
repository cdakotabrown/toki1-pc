# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile unchanged SDK clear workers behind native test adapters."""
from pathlib import Path
from safe_process import run
from pc_paths import decomp
ROOT = Path(__file__).resolve().parents[2]


def clear_objects(directory, flags=()):
    objects = []
    for name, source, defines in [
        ('sdk-clear', '_clr', ['-fcommon', '-D_clr=gpu_sdk_clear_image',
                              '-D_param=gpu_clear_get_info',
                              '-D_cwc=gpu_clear_submit']),
        ('sdk-check', 'checkRECT', ['-DcheckRECT=gpu_sdk_check_rect',
                                   '-DGPU_printf=gpu_sdk_gpu_printf']),
        ('sdk-alarm', 'set_alarm', ['-Dset_alarm=gpu_sdk_set_alarm',
                                    '-DVSync=gpu_alarm_vsync'])]:
        obj = Path(directory)/(name+'.o')
        run(['cc', '-std=gnu17', '-O1', '-fno-strict-aliasing',
             '-Ipc/include/game', '-I' + decomp('include'), *defines, *flags, '-c',
             decomp('src/main/matched/'+source+'.c'), '-o', str(obj)],
            cwd=ROOT, timeout=30).require_ok()
        objects.append(str(obj))
    return objects
