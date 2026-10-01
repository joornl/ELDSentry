What is ELDSentry?

  ELDSentry is a simple microcontroller-based firewall, built using
  off-the-shelf parts, that blocks injection of arbitrary CAN messages onto the
  SAE J1939 bus via devices connected to the SAE J1939 diagnostic port or the
  RP1226 telematics port.

  ELDSentry sits between third-party devices, like Electronic Logging Devices
  (ELDs) and the bus, filtering traffic in real time and logging CAN data to a
  microSD card for forensics.

  Electronic logging devices, mandated by the Federal Motor Carrier Safety
  Administration since 2016, connect to the diagnostic port and almost always
  carry wireless interfaces. WiFi and Bluetooth Low Energy links on those ELDs
  give attackers a path to push forged messages onto the truck's network, where
  they can interfere with vehicle control functions.

  ELDSentry was designed and developed at Oak Ridge National Laboratory with
  funding from USDOE National Nuclear Security Administration. The firmware that
  runs ELDSentry is open sourced along with a reference design for the hardware.


--

The following files are included:

(1) eldsentry.ino
      ELDSentry firmware source code

(2) eldsentry.svg
      Wiring diagram for hardware used to build/test ELDSentry

(3) hardware-BoM.txt
      Bill of Materials for hardware used to build/test ELDSentry

(4) README.txt
      This file

(5) LICENSE
      License file


--

For more information, see:

  https://eldsentry.ornl.gov

--
