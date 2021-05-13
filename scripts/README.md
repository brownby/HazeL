# Data download scripts

This directory contains two scripts that can be used for capturing the data stream from a HazeL device when it's connected to a computer via USB. When HazeL saves data to the SD card, it also send that data over the USB port if there is a device connected to it. The scripts can be used to capture this live data stream from HazeL, or to capture a bulk or incremental upload of data (described below). 

## Bulk v. incremental data uploads

The button on the HazeL (tied to the pin BUTTON_PIN, defined in [HazeL.ino](../src/HazeL.ino) initiates a data upload over USB when pressed. This upload can either be bulk or incremental mode, depending on the status of SWITCH_PIN (HIGH = bulk, LOW = incremental). 

Bulk upload means the entirety of `data.txt` is uploaded when the button is pressed. Incremental uploads only upload the portion of `data.txt` that has been collected since the *last* incremental upload. The latter can be useful when doing multiple data collection runs, using the workflow of
```
collect data for run #1 --> incremental upload --> collect data for run #2 --> incremental upload --> etc.
```
**Note:** if an incremental upload is initiated when the HazeL is not connected to anything over USB, this will **still move the indicator for the next incremental upload.**

## Script usage

### logging.sh

On Mac, data can be captured using the bash script `logging.sh`. After downloading the script, open the terminal and navigate to the directory where you downloaded it. Run the command
```
chmod 755 logging.sh
```
to make it an executable (you only need to run this once). 

Before running the script, plug a HazeL into your computer using the USB micro-B port on the Arduino MKR1000. Then, run the script:
```
./logging.sh
```
The captured data will be saved into a file tilted `HazeL_timestamp.csv`, where `timestamp` is pulled from the local machine. 

### readCOM.ps1

On Windows, data can be captured using the PowerShell script `readCOM.ps1`. After downloading the script, move it to the directory where you would like to store data. 

Before running the script, plug a HazeL into your computer using the USB micro-B port on the Arduino MKR1000. Then, right click on the script and select "Run with PowerShell". The captured data will be saved into a text file called `HazeL_data_raw.txt`.

**Note:** if the script will not run by right clicking, you may need to adjust your system's PowerShell permissions. This can be done using the `Set-ExecutionPolicy` cmdlet, more information [here](https://docs.microsoft.com/en-us/powershell/module/microsoft.powershell.core/about/about_execution_policies?view=powershell-7.1)
