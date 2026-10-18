README.txt  
OpenCL ICD Registry Repair Utility  
Version 1.0 | Supports Windows 10 / 11 (32-bit and 64-bit)  
Compatible with AMD and NVIDIA GPUs  

-------------------------------------------------------------------------------
Purpose
-------------------------------------------------------------------------------
Some systems running recent AMD or NVIDIA drivers no longer create the
Khronos OpenCL ICD registry keys that mining or compute applications depend on.
When those keys are missing, programs such as Hashwarp will display:

    No OpenCL platforms found
    Error: No usable mining devices found

This utility recreates those registry entries safely and automatically.

-------------------------------------------------------------------------------
What the Script Does
-------------------------------------------------------------------------------
1. Checks for administrative privileges and self-elevates if needed.
2. Detects whether Windows is 32-bit or 64-bit.
3. Searches the usual driver folders for OpenCL ICD libraries:
   - NVIDIA – nvopencl.dll, nvopencl64.dll, nvopencl32.dll
   - AMD – amdocl64.dll, amdocl12cl64.dll, amdocl.dll, amdocl12cl.dll
4. If a file isn’t found, it asks you to paste the full path
   (for example:
   C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_xxxxx\nvopencl64.dll)
5. Backs up any existing registry keys to your %TEMP% folder.
6. Creates or updates these registry locations:

       HKLM\SOFTWARE\Khronos\OpenCL\Vendors
       HKLM\SOFTWARE\WOW6432Node\Khronos\OpenCL\Vendors   (on 64-bit Windows)

   Each DLL path is written as a REG_DWORD 0 value as required by the
   OpenCL ICD specification.
7. Displays the final registry contents and confirms success.
8. Recommends a reboot so the driver subsystem reloads cleanly.

-------------------------------------------------------------------------------
Files in This Package
-------------------------------------------------------------------------------
hashwarp.exe         - Hashwarp executable
fix-opencl-icd.vbs   - Main script (VBScript). Double-click to run.
fix-opencl-icd.txt   - This guide.

-------------------------------------------------------------------------------
How to Use
-------------------------------------------------------------------------------
1. Run as Administrator
   - Right-click the script and select "Run".
   - If prompted with "Do you want to allow this app to make changes?",
     click "Yes". The script will restart itself with admin rights if needed.

2. Follow the prompts
   - A message will show: "Windows: 64-bit – Click OK to continue."
   - The script will search for OpenCL DLLs.
   - If not found, it will prompt you to paste paths manually.

3. Choose your vendor
   You’ll be asked:

       Register which vendor for OpenCL?
       [N] NVIDIA   [A] AMD   [B] Both   [C] Cancel

   Type N, A, or B and press OK.
   (Choose B if you have both GPUs or are unsure.)

4. Wait for confirmation
   The script will display messages such as:
       [OK] NVIDIA 64-bit ICD registered.
       [OK] NVIDIA 32-bit ICD registered.
   and finally show a summary of what was added.

5. Reboot your computer
   A reboot ensures the ICD loader sees the new entries.

-------------------------------------------------------------------------------
Verification (after reboot)
-------------------------------------------------------------------------------
1. Open PowerShell or Command Prompt.
   Run:
       reg query "HKLM\SOFTWARE\Khronos\OpenCL\Vendors"
       reg query "HKLM\SOFTWARE\WOW6432Node\Khronos\OpenCL\Vendors"

   You should see something like:
       C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_...\nvopencl64.dll    REG_DWORD    0x0

2. Run "clinfo" (if installed).
   You should now see:
       Platform Name: NVIDIA CUDA
       Device Name: NVIDIA GeForce RTX 4090
   or your AMD GPU listed.

3. Launch your mining or compute software (e.g., Hashwarp).
   It should now detect the GPU and OpenCL platform correctly.

-------------------------------------------------------------------------------
Troubleshooting
-------------------------------------------------------------------------------
Script closes immediately
    Double-clicking without admin rights.
    Fix: Right-click > Run as Administrator.

"Path does not exist" warning
    You pasted an incorrect file path.
    Fix: Verify that the DLL file exists at that location.

Still "No OpenCL platforms found"
    Reboot first. If the issue persists, reinstall your GPU driver
    and re-run the script.

Multiple GPUs (AMD + NVIDIA)
    Run again and choose B (Both).

Using DCH drivers
    DCH packages often omit registry entries; this script fixes that automatically.

-------------------------------------------------------------------------------
Backup & Safety
-------------------------------------------------------------------------------
Every run creates backups of the current registry keys:
    %TEMP%\OpenCL_Vendors_64.reg
    %TEMP%\OpenCL_Vendors_32.reg

You can restore them manually by double-clicking those .reg files if needed.

The script never deletes or overwrites DLLs—it only writes registry values.

-------------------------------------------------------------------------------
Advanced Information
-------------------------------------------------------------------------------
- The OpenCL ICD (Installable Client Driver) mechanism allows multiple
  GPU vendors to coexist.
- Each entry under the Khronos "Vendors" key is the absolute path to the
  vendor’s OpenCL runtime DLL.
- The DWORD value must be 0. Any other value disables that ICD.
- The official loader (OpenCL.dll in System32) reads these keys at runtime.

-------------------------------------------------------------------------------
Example Registry Result
-------------------------------------------------------------------------------
[HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenCL\Vendors]
"C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_d471cab2f241c3c2\nvopencl64.dll"=dword:00000000
"C:\Windows\System32\DriverStore\FileRepository\amdgpu.inf_amd64_abcdef1234567890\amdocl64.dll"=dword:00000000

-------------------------------------------------------------------------------
Final Notes
-------------------------------------------------------------------------------
- Keep GPU drivers up-to-date from NVIDIA GeForce / Studio or AMD Adrenalin.
- Re-run this tool after major driver or Windows updates if OpenCL stops working.
- Safe to distribute with mining or compute software. No installation required.

End of README
