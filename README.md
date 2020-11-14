# ES6_sensor

Dust sensor created for Harvard University ESE6, Introduction to Environmental Science and Engineering. 

Reads PM1.0, PM2.5, and PM10.0 particle concentrations and displays them on a small OLED display. Raw particle concentrations of various sizes are also read, and all data are stored on an SD card with latitude, longitude, altitude, and a timestamp of when they were collected.

Data can be uploaded to a ThingSpeak channel by defining a WiFi SSID and password, as well as a channel ID and write API key in "secrets.h". 

Designed for use with the MKR1000, also compatible with the MKR1010 by replacing the WiFi101 library with the WiFiNINA library. 
