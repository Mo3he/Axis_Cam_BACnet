# BACnet Protocol Implementation Conformance Statement (PICS)

Draft for the BACnet Server beta, written before the code so the design
stays within what BTL testing expects. Update it with every change to the
objects or services the app supports.

| Item | Value |
| --- | --- |
| Product name | BACnet Server for Axis cameras |
| Product model number | the camera's model, reported in Model_Name |
| Application software version | the ACAP version, reported in Application_Software_Version |
| Firmware revision | the AXIS OS version, reported in Firmware_Revision |
| BACnet protocol revision | 28 (bacnet-stack 1.7.0) |
| Vendor name and identifier | placeholder: "BACnet Stack at SourceForge", 260 (the library default). A released product needs its own vendor identifier from ASHRAE. |
| Product description | Exposes events from an Axis network camera (I/O, tamper, analytics, storage and other device events) as read-only BACnet values over BACnet/IP. |

## BACnet standardized device profile (Annex L)

BACnet Smart Sensor (B-SS), with the additional BIBBs below. B-SS is the
smallest profile that fits a device whose values are only read. The app
supports no writes, so profiles that require DS-WP-B (B-ASC and up) do not
apply.

## BACnet Interoperability Building Blocks supported (Annex K)

| BIBB | Name | Notes |
| --- | --- | --- |
| DS-RP-B | Data Sharing, ReadProperty, B | required by B-SS |
| DS-RPM-B | Data Sharing, ReadPropertyMultiple, B | |
| DS-COV-B | Data Sharing, COV, B | SubscribeCOV on Analog, Binary and Multi-state Value objects |
| DM-DDB-B | Device Management, Dynamic Device Binding, B | Who-Is and I-Am |
| DM-DOB-B | Device Management, Dynamic Object Binding, B | Who-Has and I-Have |

Not supported: DS-WP-B, DS-WPM-B, DM-DCC-B, DM-RD-B, DM-TS-B, DM-UTC-B,
alarm and event BIBBs, scheduling and trending.

## Segmentation capability

Segmentation is not supported. Responses that do not fit in one APDU are
rejected with the standard abort reason.

## Standard object types supported

No object can be created or deleted over BACnet. Objects are declared in the
app's settings page. Every property is read-only over BACnet.

| Object type | Creatable / deletable | Required properties | Optional properties | Writable properties |
| --- | --- | --- | --- | --- |
| Device | no | all required properties of protocol revision 28 | Description, Location, Serial_Number, Device_UUID, Local_Time, Local_Date, UTC_Offset, Daylight_Savings_Status, Active_COV_Subscriptions, Time_Of_Device_Restart, Last_Restart_Reason | none |
| Analog Value | no | Object_Identifier, Object_Name, Object_Type, Present_Value, Status_Flags, Event_State, Out_Of_Service, Units | Description, Reliability, COV_Increment, Min_Pres_Value, Max_Pres_Value | none |
| Binary Value | no | Object_Identifier, Object_Name, Object_Type, Present_Value, Status_Flags, Event_State, Out_Of_Service | Description, Reliability, Active_Text, Inactive_Text | none |
| Multi-state Value | no | Object_Identifier, Object_Name, Object_Type, Present_Value, Status_Flags, Event_State, Out_Of_Service, Number_Of_States | Description, Reliability, State_Text | none |

The values are not commandable: there is no Priority_Array or
Relinquish_Default. WriteProperty and WritePropertyMultiple are not
supported services and are rejected with UNRECOGNIZED_SERVICE.

**Reliability:** NO_FAULT_DETECTED in normal operation, and
CONFIGURATION_ERROR when the camera rejects the object's event subscription.
Status_Flags FAULT follows Reliability.

**Range restrictions:**

- Object_Name is up to 64 characters and unique within the device.
- Multi-state Value objects have 2 to 16 states.
- Up to 64 objects in total.

**Database_Revision** is stored in the app's data and increases whenever
the device settings or the object list change, so it stays the same across
restarts.

**Protocol_Services_Supported (*read from the device*):** ReadProperty,
ReadPropertyMultiple, SubscribeCOV, Who-Has and Who-Is executed; I-Am,
I-Have and UnconfirmedCOVNotification initiated.

## Data link layer options

BACnet/IP (Annex J), UDP port 47808 by default and configurable. The device
is not a BBMD and does not register as a foreign device. Other data link
layers are not supported.

## Device address binding

Dynamic binding through Who-Is and I-Am. Static binding of other devices is
not needed, because the app initiates no confirmed requests.

## Networking options

None: no routing, no BBMD, no foreign device registration, and no BACnet
network security or BACnet/SC.

## Character sets supported

UTF-8 (ANSI X3.4 compatible).

## Gateway options

Not a gateway. Values come from the camera's own event system, not from
other networks.
