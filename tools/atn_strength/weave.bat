@echo off
REM ATN woven-wall post-processing wrapper for ATN Slicer / OrcaSlicer.
REM
REM OrcaSlicer's post-processing runner checks that the FIRST token of the field
REM exists on disk and only knows how to launch .pl (perl) and .bat files -- it
REM cannot resolve a bare "python". So point the Post-processing Scripts field at
REM the FULL PATH of this .bat instead, e.g.:
REM
REM   "C:\Users\Graham Work\OrcaSlicer\tools\atn_strength\weave.bat" --amp 0.3 --wavelength 4
REM
REM OrcaSlicer then runs:  cmd.exe /C "<this.bat>" <your args> "<gcode path>"
REM and this forwards everything (incl. the gcode path as the last arg) to the
REM Python script, which edits the gcode in place.
REM
REM Uses the Windows "py" launcher (system-installed, stable). If you prefer a
REM specific interpreter, replace the py.exe path below with your python.exe.
"C:\Users\Graham Work\AppData\Local\Programs\Python\Launcher\py.exe" -3 "%~dp0weave_post.py" %*
