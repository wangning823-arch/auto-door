' 双击启动 OTA 客户端（无黑框）
Option Explicit
Dim sh, env, py, script, cwd
Set sh = CreateObject("WScript.Shell")
Set env = sh.Environment("PROCESS")
cwd = CreateObject("Scripting.FileSystemObject").GetParentFolderName(WScript.ScriptFullName)
script = cwd & "\ota_client.py"
py = ""
If env("MIMO_PYTHON") <> "" Then
  py = env("MIMO_PYTHON")
End If
If py = "" Then
  py = "python"
End If
sh.CurrentDirectory = cwd
sh.Run """" & py & """ """ & script & """", 0, False
