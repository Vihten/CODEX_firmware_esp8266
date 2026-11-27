Option Explicit
Dim shell, files, scriptDir, command, arg
Set shell = CreateObject("WScript.Shell")
Set files = CreateObject("Scripting.FileSystemObject")
scriptDir = files.GetParentFolderName(WScript.ScriptFullName)
command = "pythonw.exe -B """ & files.BuildPath(scriptDir, "hub_ui.py") & """"
For Each arg In WScript.Arguments
    If arg <> "--start" Then WScript.Quit 2
    command = command & " --start"
Next
On Error Resume Next
shell.Run command, 1, False
If Err.Number <> 0 Then
    MsgBox "Python 3.10+ with Tk and pythonw.exe on PATH is required.", vbExclamation, "Agent Hub"
    WScript.Quit 1
End If
