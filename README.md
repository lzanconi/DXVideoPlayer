## DirectX VideoPlayer
### Install Visual Studio project dependencies
The project has only three dependencies:
- **Windows SDK**
- **FFmpeg 8.1** or greater
- **nlohmann json**

***Install Windows SDK***
1. Open **Visual Studio Installer**
2. Click **Modify** on your current installation
3. Check the box for **Game development with C++** (this installs the necessary C++ tools, compiler, and the latest Windows SDK)
4. Click **Modify/Install** to apply changes

***Install FFmpeg 8.1***
1. Dowload this [file](https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip)
2. Unzip it
3. Rename the folder to something like **ffmpeg-dev** and put it to folder wherever you like: (e.g C:\ffmpeg-dev) 

***Install nlohmann json***
1. Download this [file](https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp)
2. Put it wherever you want (e.g. C:\nlohmann\json.hpp)

### Visual Studio project update Include paths
To update include paths, right click on the project name in the **Solution Explorer** and click on **Properties**
Go to **C/C++** -> **Additional Include Directories**, here you can update the include paths

### Visual Studio project update linker 
To update linker paths, right click on the project name in the **Solution Explorer** and click on **Properties**
Go to **Linker** -> **General** -> **Additional Library Directories**, here you can update the libraries paths
Go to **Linker** -> **Input** -> **Additional Depedencies**, here you can pase the libraries file names


```
# 1. Remove any previous task with the same name
Unregister-ScheduledTask -TaskName "RunDXPlayer" -Confirm:$false 

# 2. Set your paths 
$AppFolder = "C:\Users\LattePanda\DXVideoPlayer" 
$LogFile = "$AppFolder\output.log" 
$TargetUser = "LattePanda" # The user currently logged into the desktop

# 3. Create an action that launches the app and redirects output 
# We use -WindowStyle Hidden so a messy PowerShell window doesn't pop up on the desktop user 
$Arguments = "-Command & { cd '$AppFolder'; .\DXVideoPlayer.exe > '$LogFile' 2>&1 }" 
$Action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument $Arguments

# 4. Register the task 
Register-ScheduledTask -TaskName "RunDXPlayer" -User $TargetUser -Action $Action
```
To run the script, open a powershell terminal and run it:
```
.\Create-DXPlayer-Scheduled-Task.ps1
```
The first time you run this script it will return an error: *Unregister-ScheduledTask : No MSFT_ScheduledTask objects found with property 'TaskName' equal to 'RunDXPlayer'.*
Don't worry, it just the first command in the script that fails because it can Unregister a task that doesn't exist yet.

Now the Scheduled Task should have been created, to run it (from powershell) run:
```
Start-ScheduledTask -TaskName "RunDXPlayer"
```
To view the output of the application, run:
```
Get-Content "C:\Users\LattePanda\DXVideoPlayer\output.log" -Wait
```
Finally, to kill the app, enter in powershell and run the Scheduled Task:
```
Start-ScheduledTask -Name "CloseDXPlayer"
```
Use **Ctrl + C** to exit from viewing the output
schtasks /create /tn "CloseDXPlayer" /tr "powershell.exe -WindowStyle Hidden -File C:\Users\LattePanda\DXVideoPlayer\Close-DXVideoPlayer.ps1" /sc ONCE /sd 01/01/2026 /st 00:00 /it /ru "Builtin\Users" /f
```
```
To register this Scheduled Task, exit from powershell and run in the SSH terminal:
```

**3.Kill the app from SSH**
$w = New-Object -ComObject WScript.Shell; if ($w.AppActivate("DirectX Video Player")) { Start-Sleep -Milliseconds 200; $w.SendKeys("{ESC}") }      
Running **taskkill** is not enough, it kills the app but it leaves some child threads alive that causes the app to hangs when you run it again after trying to kill it.
First we have to create a script named **Close-DXVideoPlayer.ps1** and paste the following code:
```
### Setup Windows 11 to run app from SSH
Create a powershell script and name it **Create-DXPlayer-Scheduled-Task.ps1**
Open the file and paste the following code:
**1.Enable Administrator Elevation for OpenSSH**
In Windows, SSH sessions run in a secure, isolated background space known as **Session 0**, this prevents sessions from interacting with the desktop (which runs in **Session 1**).
The cleanest way to overcome this problem is to use **Windows Task Scheduler**, because Scheduled Task can be configured to run in the context of the currently logged-in desktop user on the remote machine.
By default, Windows SSH sessions log you with standard user permission even if your user is part of Administration group.
To grant the user full administrative token via SSH, open a **power shell** terminal with Administration rights and then run this command:
```New-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" -Name "LocalAccountTokenFilterPolicy" -Value 1 -PropertyType DWORD -Force```
