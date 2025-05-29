import collections
import ctypes
import datetime
import enum
import math
import platform
import socket


class Mode(enum.Enum):
    FM = 0
    AM = 1


class EventType(enum.Enum):
    LOST_DEVICE = 0
    IQ = 1
    SYNC = 2
    LOST_SYNC = 3
    MER = 4
    BER = 5
    HDC = 6
    AUDIO = 7
    ID3 = 8
    SIG = 9
    LOT = 10
    SIS = 11
    STREAM = 12
    PACKET = 13
    AUDIO_SERVICE = 14
    STATION_ID = 15
    STATION_NAME = 16
    STATION_SLOGAN = 17
    STATION_MESSAGE = 18
    STATION_LOCATION = 19
    AUDIO_SERVICE_DESCRIPTOR = 20
    DATA_SERVICE_DESCRIPTOR = 21
    EMERGENCY_ALERT = 22


AUDIO_FRAME_SAMPLES = 2048

SAMPLE_RATE_CU8 = 1488375
SAMPLE_RATE_CS16_FM = 744187.5
SAMPLE_RATE_CS16_AM = 46511.71875
SAMPLE_RATE_AUDIO = 44100


class ServiceType(enum.Enum):
    AUDIO = 0
    DATA = 1


class ComponentType(enum.Enum):
    AUDIO = 0
    DATA = 1


class MIMEType(enum.Enum):
    PRIMARY_IMAGE = 0xBE4B7536
    STATION_LOGO = 0xD9C72536
    NAVTEQ = 0x2D42AC3E
    HERE_TPEG = 0x82F03DFC
    HERE_IMAGE = 0xB7F03DFC
    HD_TMC = 0xEECB55B6
    HDC = 0x4DC66C5A
    TEXT = 0xBB492AAC
    JPEG = 0x1E653E9C
    PNG = 0x4F328CA0
    TTN_TPEG_1 = 0xB39EBEB2
    TTN_TPEG_2 = 0x4EB03469
    TTN_TPEG_3 = 0x52103469
    TTN_STM_TRAFFIC = 0xFF8422D7
    TTN_STM_WEATHER = 0xEF042E96
    UNKNOWN_00000000 = 0x00000000
    UNKNOWN_B81FFAA8 = 0xB81FFAA8
    UNKNOWN_FFFFFFFF = 0xFFFFFFFF


class Access(enum.Enum):
    PUBLIC = 0
    RESTRICTED = 1


class ServiceDataType(enum.Enum):
    NON_SPECIFIC = 0
    NEWS = 1
    SPORTS = 3
    WEATHER = 29
    EMERGENCY = 31
    TRAFFIC = 65
    IMAGE_MAPS = 66
    TEXT = 80
    ADVERTISING = 256
    FINANCIAL = 257
    STOCK_TICKER = 258
    NAVIGATION = 259
    ELECTRONIC_PROGRAM_GUIDE = 260
    AUDIO = 261
    PRIVATE_DATA_NETWORK = 262
    SERVICE_MAINTENANCE = 263
    HD_RADIO_SYSTEM_SERVICES = 264
    AUDIO_RELATED_DATA = 265
    RESERVED_FOR_SPECIAL_TESTS = 511


class ProgramType(enum.Enum):
    UNDEFINED = 0
    NEWS = 1
    INFORMATION = 2
    SPORTS = 3
    TALK = 4
    ROCK = 5
    CLASSIC_ROCK = 6
    ADULT_HITS = 7
    SOFT_ROCK = 8
    TOP_40 = 9
    COUNTRY = 10
    OLDIES = 11
    SOFT = 12
    NOSTALGIA = 13
    JAZZ = 14
    CLASSICAL = 15
    RHYTHM_AND_BLUES = 16
    SOFT_RHYTHM_AND_BLUES = 17
    FOREIGN_LANGUAGE = 18
    RELIGIOUS_MUSIC = 19
    RELIGIOUS_TALK = 20
    PERSONALITY = 21
    PUBLIC = 22
    COLLEGE = 23
    SPANISH_TALK = 24
    SPANISH_MUSIC = 25
    HIP_HOP = 26
    WEATHER = 29
    EMERGENCY_TEST = 30
    EMERGENCY = 31
    TRAFFIC = 65
    SPECIAL_READING_SERVICES = 76


class Blend(enum.Enum):
    DISABLE = 0
    SELECT = 1
    ENABLE = 2


class LocationFormat(enum.Enum):
    SAME = 0
    FIPS = 1
    ZIP = 2


class AlertCategory(enum.Enum):
    NON_SPECIFIC = 1
    GEOPHYSICAL = 2
    WEATHER = 3
    SAFETY = 4
    SECURITY = 5
    RESCUE = 6
    FIRE = 7
    HEALTH = 8
    ENVIRONMENTAL = 9
    TRANSPORTATION = 10
    UTILITIES = 11
    HAZMAT = 12
    TEST = 30


IQ = collections.namedtuple("IQ", ["data"])
MER = collections.namedtuple("MER", ["lower", "upper"])
BER = collections.namedtuple("BER", ["cber"])
HDC = collections.namedtuple("HDC", ["program", "data"])
Audio = collections.namedtuple("Audio", ["program", "data"])
Comment = collections.namedtuple("Comment", ["lang", "short_content_desc", "full_text"])
UFID = collections.namedtuple("UFID", ["owner", "id"])
XHDR = collections.namedtuple("XHDR", ["mime", "param", "lot"])
ID3 = collections.namedtuple("ID3", ["program", "title", "artist", "album", "genre", "ufid", "xhdr", "comments"])
SIGAudioComponent = collections.namedtuple("SIGAudioComponent", ["port", "type", "mime"])
SIGDataComponent = collections.namedtuple("SIGDataComponent", ["port", "service_data_type", "type", "mime"])
SIGComponent = collections.namedtuple("SIGComponent", ["type", "id", "audio", "data"])
SIGService = collections.namedtuple("SIGService", ["type", "number", "name", "components"])
SIG = collections.namedtuple("SIG", ["services"])
STREAM = collections.namedtuple("STREAM", ["port", "seq", "mime", "data", "service", "component"])
PACKET = collections.namedtuple("PACKET", ["port", "seq", "mime", "data", "service", "component"])
LOT = collections.namedtuple("LOT", ["port", "lot", "mime", "name", "data", "expiry_utc", "service", "component"])
SISAudioService = collections.namedtuple("SISAudioService", ["program", "access", "type", "sound_exp"])
SISDataService = collections.namedtuple("SISDataService", ["access", "type", "mime_type"])
SIS = collections.namedtuple("SIS", ["country_code", "fcc_facility_id", "name", "slogan", "message", "alert",
                                     "latitude", "longitude", "altitude", "audio_services", "data_services",
                                     "alert_cnt", "alert_categories", "alert_location_format", "alert_locations"])
StationID = collections.namedtuple("StationID", ["country_code", "fcc_facility_id"])
StationName = collections.namedtuple("StationName", ["name"])
StationSlogan = collections.namedtuple("StationSlogan", ["slogan"])
StationMessage = collections.namedtuple("StationMessage", ["message"])
StationLocation = collections.namedtuple("StationLocation", ["latitude", "longitude", "altitude"])
EmergencyAlert = collections.namedtuple("EmergencyAlert", ["message", "control_data", "categories", "location_format", "locations"])
AudioService = collections.namedtuple("AudioService", ["program", "access", "type", "codec_mode", "blend_control",
                                                       "digital_audio_gain", "common_delay", "latency"])


class _IQ(ctypes.Structure):
    _fields_ = [
        ("data", ctypes.POINTER(ctypes.c_char)),
        ("count", ctypes.c_size_t),
    ]


class _MER(ctypes.Structure):
    _fields_ = [
        ("lower", ctypes.c_float),
        ("upper", ctypes.c_float),
    ]


class _BER(ctypes.Structure):
    _fields_ = [
        ("cber", ctypes.c_float),
    ]


class _HDC(ctypes.Structure):
    _fields_ = [
        ("program", ctypes.c_uint),
        ("data", ctypes.POINTER(ctypes.c_char)),
        ("count", ctypes.c_size_t),
    ]


class _Audio(ctypes.Structure):
    _fields_ = [
        ("program", ctypes.c_uint),
        ("data", ctypes.POINTER(ctypes.c_char)),
        ("count", ctypes.c_size_t),
    ]

class _Comment(ctypes.Structure):
    pass

_Comment._fields_ = [
    ("next", ctypes.POINTER(_Comment)),
    ("lang", ctypes.c_char_p),
    ("short_content_desc", ctypes.c_char_p),
    ("full_text", ctypes.c_char_p),
]


class _UFID(ctypes.Structure):
    _fields_ = [
        ("owner", ctypes.c_char_p),
        ("id", ctypes.c_char_p),
    ]


class _XHDR(ctypes.Structure):
    _fields_ = [
        ("mime", ctypes.c_uint32),
        ("param", ctypes.c_int),
        ("lot", ctypes.c_int),
    ]


class _ID3(ctypes.Structure):
    _fields_ = [
        ("program", ctypes.c_uint),
        ("title", ctypes.c_char_p),
        ("artist", ctypes.c_char_p),
        ("album", ctypes.c_char_p),
        ("genre", ctypes.c_char_p),
        ("ufid", _UFID),
        ("xhdr", _XHDR),
        ("comments", ctypes.POINTER(_Comment)),
    ]


class _SIGData(ctypes.Structure):
    _fields_ = [
        ("port", ctypes.c_uint16),
        ("service_data_type", ctypes.c_uint16),
        ("type", ctypes.c_uint8),
        ("mime", ctypes.c_uint32),
    ]


class _SIGAudio(ctypes.Structure):
    _fields_ = [
        ("port", ctypes.c_uint8),
        ("type", ctypes.c_uint8),
        ("mime", ctypes.c_uint32),
    ]


class _SIGUnion(ctypes.Union):
    _fields_ = [
        ("audio", _SIGAudio),
        ("data", _SIGData),
    ]


class _SIGComponent(ctypes.Structure):
    pass


_SIGComponent._fields_ = [
    ("next", ctypes.POINTER(_SIGComponent)),
    ("type", ctypes.c_uint8),
    ("id", ctypes.c_uint8),
    ("u", _SIGUnion),
]


class _SIGService(ctypes.Structure):
    pass


_SIGService._fields_ = [
    ("next", ctypes.POINTER(_SIGService)),
    ("type", ctypes.c_uint8),
    ("number", ctypes.c_uint16),
    ("name", ctypes.c_char_p),
    ("components", ctypes.POINTER(_SIGComponent)),
]


class _SIG(ctypes.Structure):
    _fields_ = [
        ("services", ctypes.POINTER(_SIGService)),
    ]


class _STREAM(ctypes.Structure):
    _fields_ = [
        ("port", ctypes.c_uint16),
        ("seq", ctypes.c_uint16),
        ("size", ctypes.c_uint),
        ("mime", ctypes.c_uint32),
        ("data", ctypes.POINTER(ctypes.c_char)),
        ("service", ctypes.POINTER(_SIGService)),
        ("component", ctypes.POINTER(_SIGComponent)),
    ]


class _PACKET(ctypes.Structure):
    _fields_ = [
        ("port", ctypes.c_uint16),
        ("seq", ctypes.c_uint16),
        ("size", ctypes.c_uint),
        ("mime", ctypes.c_uint32),
        ("data", ctypes.POINTER(ctypes.c_char)),
        ("service", ctypes.POINTER(_SIGService)),
        ("component", ctypes.POINTER(_SIGComponent)),
    ]


class _TimeStruct(ctypes.Structure):
    _fields_ = [
        ("tm_sec", ctypes.c_int),
        ("tm_min", ctypes.c_int),
        ("tm_hour", ctypes.c_int),
        ("tm_mday", ctypes.c_int),
        ("tm_mon", ctypes.c_int),
        ("tm_year", ctypes.c_int),
        ("tm_wday", ctypes.c_int),
        ("tm_yday", ctypes.c_int),
        ("tm_isdst", ctypes.c_int),
    ]


class _LOT(ctypes.Structure):
    _fields_ = [
        ("port", ctypes.c_uint16),
        ("lot", ctypes.c_uint),
        ("size", ctypes.c_uint),
        ("mime", ctypes.c_uint32),
        ("name", ctypes.c_char_p),
        ("data", ctypes.POINTER(ctypes.c_char)),
        ("expiry_utc", ctypes.POINTER(_TimeStruct)),
        ("service", ctypes.POINTER(_SIGService)),
        ("component", ctypes.POINTER(_SIGComponent)),
    ]


class _SISAudioService(ctypes.Structure):
    pass


_SISAudioService._fields_ = [
    ("next", ctypes.POINTER(_SISAudioService)),
    ("program", ctypes.c_uint),
    ("access", ctypes.c_uint),
    ("type", ctypes.c_uint),
    ("sound_exp", ctypes.c_uint),
]


class _SISDataService(ctypes.Structure):
    pass


_SISDataService._fields_ = [
    ("next", ctypes.POINTER(_SISDataService)),
    ("access", ctypes.c_uint),
    ("type", ctypes.c_uint),
    ("mime_type", ctypes.c_uint32),
]


class _SIS(ctypes.Structure):
    _fields_ = [
        ("country_code", ctypes.c_char_p),
        ("fcc_facility_id", ctypes.c_int),
        ("name", ctypes.c_char_p),
        ("slogan", ctypes.c_char_p),
        ("message", ctypes.c_char_p),
        ("alert", ctypes.c_char_p),
        ("latitude", ctypes.c_float),
        ("longitude", ctypes.c_float),
        ("altitude", ctypes.c_int),
        ("audio_services", ctypes.POINTER(_SISAudioService)),
        ("data_services", ctypes.POINTER(_SISDataService)),
        ("alert_cnt", ctypes.POINTER(ctypes.c_char)),
        ("alert_cnt_length", ctypes.c_int),
        ("alert_category1", ctypes.c_int),
        ("alert_category2", ctypes.c_int),
        ("alert_location_format", ctypes.c_int),
        ("alert_num_locations", ctypes.c_int),
        ("alert_locations", ctypes.POINTER(ctypes.c_int)),
    ]


class _StationID(ctypes.Structure):
    _fields_ = [
        ("country_code", ctypes.c_char_p),
        ("fcc_facility_id", ctypes.c_int),
    ]


class _StationName(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
    ]


class _StationSlogan(ctypes.Structure):
    _fields_ = [
        ("slogan", ctypes.c_char_p),
    ]


class _StationMessage(ctypes.Structure):
    _fields_ = [
        ("message", ctypes.c_char_p),
    ]


class _StationLocation(ctypes.Structure):
    _fields_ = [
        ("latitude", ctypes.c_float),
        ("longitude", ctypes.c_float),
        ("altitude", ctypes.c_int),
    ]


class _ASD(ctypes.Structure):
    _fields_ = [
    ("program", ctypes.c_uint),
    ("access", ctypes.c_uint),
    ("type", ctypes.c_uint),
    ("sound_exp", ctypes.c_uint),
]


class _DSD(ctypes.Structure):
    _fields_ = [
    ("access", ctypes.c_uint),
    ("type", ctypes.c_uint),
    ("mime_type", ctypes.c_uint32),
]


class _EmergencyAlert(ctypes.Structure):
    _fields_ = [
        ("message", ctypes.c_char_p),
        ("control_data", ctypes.POINTER(ctypes.c_char)),
        ("control_data_length", ctypes.c_int),
        ("category1", ctypes.c_int),
        ("category2", ctypes.c_int),
        ("location_format", ctypes.c_int),
        ("num_locations", ctypes.c_int),
        ("locations", ctypes.POINTER(ctypes.c_int)),
    ]


class _AudioService(ctypes.Structure):
    _fields_ = [
        ("program", ctypes.c_uint),
        ("access", ctypes.c_uint),
        ("type", ctypes.c_uint),
        ("codec_mode", ctypes.c_uint),
        ("blend_control", ctypes.c_uint),
        ("digital_audio_gain", ctypes.c_int),
        ("common_delay", ctypes.c_uint),
        ("latency", ctypes.c_uint),
    ]


class _EventUnion(ctypes.Union):
    _fields_ = [
        ("iq", _IQ),
        ("mer", _MER),
        ("ber", _BER),
        ("hdc", _HDC),
        ("audio", _Audio),
        ("id3", _ID3),
        ("sig", _SIG),
        ("stream", _STREAM),
        ("packet", _PACKET),
        ("lot", _LOT),
        ("sis", _SIS),
        ("station_id", _StationID),
        ("station_name", _StationName),
        ("station_slogan", _StationSlogan),
        ("station_message", _StationMessage),
        ("station_location", _StationLocation),
        ("asd", _ASD),
        ("dsd", _DSD),
        ("emergency_alert", _EmergencyAlert),
        ("audio_service", _AudioService)
    ]


class _Event(ctypes.Structure):
    _fields_ = [
        ("event", ctypes.c_uint),
        ("u", _EventUnion),
    ]


class NRSC5Error(Exception):
    pass


class NRSC5:
    libnrsc5 = None

    def _load_library(self):
        if NRSC5.libnrsc5 is None:
            if platform.system() == "Windows":
                lib_name = "libnrsc5.dll"
            elif platform.system() == "Linux":
                lib_name = "libnrsc5.so"
            elif platform.system() == "Darwin":
                lib_name = "libnrsc5.dylib"
            else:
                raise NRSC5Error("Unsupported platform: " + platform.system())
            NRSC5.libnrsc5 = ctypes.cdll.LoadLibrary(lib_name)
            self.radio = ctypes.c_void_p()

    @staticmethod
    def _decode(string):
        if string is None:
            return string
        return string.decode()

    def _callback_wrapper(self, c_evt):
        c_evt = c_evt.contents
        evt = None

        try:
            evt_type = EventType(c_evt.event)
        except ValueError:
            return

        if evt_type == EventType.IQ:
            iq = c_evt.u.iq
            evt = IQ(iq.data[:iq.count])
        elif evt_type == EventType.MER:
            mer = c_evt.u.mer
            evt = MER(mer.lower, mer.upper)
        elif evt_type == EventType.BER:
            ber = c_evt.u.ber
            evt = BER(ber.cber)
        elif evt_type == EventType.HDC:
            hdc = c_evt.u.hdc
            evt = HDC(hdc.program, hdc.data[:hdc.count])
        elif evt_type == EventType.AUDIO:
            audio = c_evt.u.audio
            evt = Audio(audio.program, audio.data[:audio.count * 2])
        elif evt_type == EventType.ID3:
            id3 = c_evt.u.id3

            ufid = None
            if id3.ufid.owner or id3.ufid.id:
                ufid = UFID(self._decode(id3.ufid.owner), self._decode(id3.ufid.id))

            xhdr = None
            if id3.xhdr.mime != 0 or id3.xhdr.param != -1 or id3.xhdr.lot != -1:
                xhdr = XHDR(None if id3.xhdr.mime == 0 else MIMEType(id3.xhdr.mime),
                            None if id3.xhdr.param == -1 else id3.xhdr.param,
                            None if id3.xhdr.lot == -1 else id3.xhdr.lot)
            comments = []
            comment_ptr = id3.comments
            while comment_ptr:
                c = comment_ptr.contents
                comments.append(Comment(self._decode(c.lang), self._decode(c.short_content_desc), self._decode(c.full_text)))
                comment_ptr = c.next

            evt = ID3(id3.program, self._decode(id3.title), self._decode(id3.artist),
                      self._decode(id3.album), self._decode(id3.genre), ufid, xhdr, comments)
        elif evt_type == EventType.SIG:
            evt = []
            self.services = {}
            self.components = {}
            service_ptr = c_evt.u.sig.services
            while service_ptr:
                service = service_ptr.contents
                components = []
                component_ptr = service.components
                while component_ptr:
                    component = component_ptr.contents
                    component_type = ComponentType(component.type)
                    audio = None
                    data = None
                    if component_type == ComponentType.AUDIO:
                        audio = SIGAudioComponent(component.u.audio.port, ProgramType(component.u.audio.type),
                                                  MIMEType(component.u.audio.mime))
                    if component_type == ComponentType.DATA:
                        data = SIGDataComponent(component.u.data.port,
                                                ServiceDataType(component.u.data.service_data_type),
                                                component.u.data.type, MIMEType(component.u.data.mime))
                    component_obj = SIGComponent(component_type, component.id, audio, data)
                    components.append(component_obj)
                    self.components[(service.number, component.id)] = component_obj
                    component_ptr = component.next
                service_obj = SIGService(ServiceType(service.type), service.number,
                                         self._decode(service.name), components)
                evt.append(service_obj)
                self.services[service.number] = service_obj
                service_ptr = service.next
        elif evt_type == EventType.STREAM:
            stream = c_evt.u.stream
            service = self.services[stream.service.contents.number]
            component = self.components[(stream.service.contents.number, stream.component.contents.id)]
            evt = STREAM(stream.port, stream.seq, MIMEType(component.data.mime), stream.data[:stream.size], service, component)
        elif evt_type == EventType.PACKET:
            packet = c_evt.u.packet
            service = self.services[packet.service.contents.number]
            component = self.components[(packet.service.contents.number, packet.component.contents.id)]
            evt = PACKET(packet.port, packet.seq, MIMEType(component.data.mime), packet.data[:packet.size], service, component)
        elif evt_type == EventType.LOT:
            lot = c_evt.u.lot
            service = self.services[lot.service.contents.number]
            component = self.components[(lot.service.contents.number, lot.component.contents.id)]
            expiry_struct = lot.expiry_utc.contents
            expiry_time = datetime.datetime(
                expiry_struct.tm_year + 1900,
                expiry_struct.tm_mon + 1,
                expiry_struct.tm_mday,
                expiry_struct.tm_hour,
                expiry_struct.tm_min,
                expiry_struct.tm_sec,
                tzinfo=datetime.timezone.utc
            )
            evt = LOT(lot.port, lot.lot, MIMEType(lot.mime), self._decode(lot.name), lot.data[:lot.size], expiry_time, service, component)
        elif evt_type == EventType.SIS:
            sis = c_evt.u.sis

            latitude, longitude, altitude = None, None, None
            if not math.isnan(sis.latitude):
                latitude, longitude, altitude = sis.latitude, sis.longitude, sis.altitude

            audio_services = []
            audio_service_ptr = sis.audio_services
            while audio_service_ptr:
                asd = audio_service_ptr.contents
                audio_services.append(SISAudioService(asd.program, Access(asd.access),
                                                      ProgramType(asd.type), asd.sound_exp))
                audio_service_ptr = asd.next

            data_services = []
            data_service_ptr = sis.data_services
            while data_service_ptr:
                dsd = data_service_ptr.contents
                data_services.append(SISDataService(Access(dsd.access), ServiceDataType(dsd.type), dsd.mime_type))
                data_service_ptr = dsd.next

            alert_categories = []
            if sis.alert_category1 >= 1:
                alert_categories.append(AlertCategory(sis.alert_category1))
            if sis.alert_category2 >= 1:
                alert_categories.append(AlertCategory(sis.alert_category2))
            evt = SIS(self._decode(sis.country_code), sis.fcc_facility_id, self._decode(sis.name),
                      self._decode(sis.slogan), self._decode(sis.message), self._decode(sis.alert),
                      latitude, longitude, altitude, audio_services, data_services,
                      sis.alert_cnt[:sis.alert_cnt_length], alert_categories,
                      None if sis.alert_location_format == -1 else LocationFormat(sis.alert_location_format),
                      sis.alert_locations[:sis.alert_num_locations])
        elif evt_type == EventType.STATION_ID:
            station_id = c_evt.u.station_id
            evt = StationID(self._decode(station_id.country_code), station_id.fcc_facility_id)
        elif evt_type == EventType.STATION_NAME:
            station_name = c_evt.u.station_name
            evt = StationName(self._decode(station_name.name))
        elif evt_type == EventType.STATION_SLOGAN:
            station_slogan = c_evt.u.station_slogan
            evt = StationSlogan(self._decode(station_slogan.slogan))
        elif evt_type == EventType.STATION_MESSAGE:
            station_message = c_evt.u.station_message
            evt = StationMessage(self._decode(station_message.message))
        elif evt_type == EventType.STATION_LOCATION:
            station_location = c_evt.u.station_location
            evt = StationLocation(station_location.latitude, station_location.longitude, station_location.altitude)
        elif evt_type == EventType.AUDIO_SERVICE_DESCRIPTOR:
            asd = c_evt.u.asd
            evt = SISAudioService(asd.program, Access(asd.access), ProgramType(asd.type), asd.sound_exp)
        elif evt_type == EventType.DATA_SERVICE_DESCRIPTOR:
            dsd = c_evt.u.dsd
            evt = SISDataService(Access(dsd.access), ServiceDataType(dsd.type), dsd.mime_type)
        elif evt_type == EventType.EMERGENCY_ALERT:
            emergency_alert = c_evt.u.emergency_alert
            categories = []
            if emergency_alert.category1 >= 1:
                categories.append(AlertCategory(emergency_alert.category1))
            if emergency_alert.category2 >= 1:
                categories.append(AlertCategory(emergency_alert.category2))
            evt = EmergencyAlert(self._decode(emergency_alert.message),
                                 emergency_alert.control_data[:emergency_alert.control_data_length],
                                 categories,
                                 None if emergency_alert.location_format == -1 else LocationFormat(emergency_alert.location_format),
                                 emergency_alert.locations[:emergency_alert.num_locations])

        elif evt_type == EventType.AUDIO_SERVICE:
            service = c_evt.u.audio_service
            evt = AudioService(
                service.program,
                Access(service.access),
                ProgramType(service.type),
                service.codec_mode,
                Blend(service.blend_control),
                service.digital_audio_gain,
                service.common_delay,
                service.latency
            )

        self.callback(evt_type, evt, *self.callback_args)

    def __init__(self, callback, callback_args=()):
        self._load_library()
        self.radio = ctypes.c_void_p()
        self.callback = callback
        self.callback_args = callback_args

    @staticmethod
    def get_version():
        version = ctypes.c_char_p()
        NRSC5.libnrsc5.nrsc5_get_version(ctypes.byref(version))
        return version.value.decode()

    @staticmethod
    def service_data_type_name(service_data_type):
        name = ctypes.c_char_p()
        NRSC5.libnrsc5.nrsc5_service_data_type_name(service_data_type.value, ctypes.byref(name))
        return name.value.decode()

    @staticmethod
    def program_type_name(program_type):
        name = ctypes.c_char_p()
        NRSC5.libnrsc5.nrsc5_program_type_name(program_type.value, ctypes.byref(name))
        return name.value.decode()

    @staticmethod
    def alert_category_name(category):
        name = ctypes.c_char_p()
        NRSC5.libnrsc5.nrsc5_alert_category_name(category.value, ctypes.byref(name))
        return name.value.decode()

    def open(self, device_index):
        result = NRSC5.libnrsc5.nrsc5_open(ctypes.byref(self.radio), device_index)
        if result != 0:
            raise NRSC5Error("Failed to open RTL-SDR.")
        self._set_callback()

    def open_pipe(self):
        result = NRSC5.libnrsc5.nrsc5_open_pipe(ctypes.byref(self.radio))
        if result != 0:
            raise NRSC5Error("Failed to open pipe.")
        self._set_callback()

    def open_rtltcp(self, host, port):
        s = socket.create_connection((host, port))
        result = NRSC5.libnrsc5.nrsc5_open_rtltcp(ctypes.byref(self.radio), s.detach())
        if result != 0:
            raise NRSC5Error("Failed to open rtl_tcp.")
        self._set_callback()
    
    def _check_session(self):
        if not self.radio:
            raise NRSC5Error("No session opened. Call open(), open_pipe(), or open_rtltcp() first.")

    def close(self):
        self._check_session()
        NRSC5.libnrsc5.nrsc5_close(self.radio)
        self.radio = ctypes.c_void_p()

    def start(self):
        self._check_session()
        NRSC5.libnrsc5.nrsc5_start(self.radio)

    def stop(self):
        self._check_session()
        NRSC5.libnrsc5.nrsc5_stop(self.radio)

    def set_mode(self, mode):
        self._check_session()
        NRSC5.libnrsc5.nrsc5_set_mode(self.radio, mode.value)

    def set_bias_tee(self, on):
        self._check_session()
        result = NRSC5.libnrsc5.nrsc5_set_bias_tee(self.radio, on)
        if result != 0:
            raise NRSC5Error("Failed to set bias-T.")

    def set_direct_sampling(self, on):
        self._check_session()
        result = NRSC5.libnrsc5.nrsc5_set_direct_sampling(self.radio, on)
        if result != 0:
            raise NRSC5Error("Failed to set direct sampling.")

    def set_freq_correction(self, ppm_error):
        self._check_session()
        result = NRSC5.libnrsc5.nrsc5_set_freq_correction(self.radio, ppm_error)
        if result != 0:
            raise NRSC5Error("Failed to set frequency correction.")

    def get_frequency(self):
        self._check_session()
        frequency = ctypes.c_float()
        NRSC5.libnrsc5.nrsc5_get_frequency(self.radio, ctypes.byref(frequency))
        return frequency.value

    def set_frequency(self, freq):
        self._check_session()
        result = NRSC5.libnrsc5.nrsc5_set_frequency(self.radio, ctypes.c_float(freq))
        if result != 0:
            raise NRSC5Error("Failed to set frequency.")

    def get_gain(self):
        self._check_session()
        gain = ctypes.c_float()
        NRSC5.libnrsc5.nrsc5_get_gain(self.radio, ctypes.byref(gain))
        return gain.value

    def set_gain(self, gain):
        self._check_session()
        result = NRSC5.libnrsc5.nrsc5_set_gain(self.radio, ctypes.c_float(gain))
        if result != 0:
            raise NRSC5Error("Failed to set gain.")

    def set_auto_gain(self, enabled):
        self._check_session()
        NRSC5.libnrsc5.nrsc5_set_auto_gain(self.radio, int(enabled))

    def _set_callback(self):
        def callback_closure(evt, opaque):
            self._callback_wrapper(evt)

        self.callback_func = ctypes.CFUNCTYPE(None, ctypes.POINTER(_Event), ctypes.c_void_p)(callback_closure)
        NRSC5.libnrsc5.nrsc5_set_callback(self.radio, self.callback_func, None)

    def pipe_samples_cu8(self, samples):
        result = NRSC5.libnrsc5.nrsc5_pipe_samples_cu8(self.radio, samples, len(samples))
        if result != 0:
            raise NRSC5Error("Failed to pipe samples.")

    def pipe_samples_cs16(self, samples):
        if len(samples) % 2 != 0:
            raise NRSC5Error("len(samples) must be a multiple of 2.")
        result = NRSC5.libnrsc5.nrsc5_pipe_samples_cs16(self.radio, samples, len(samples) // 2)
        if result != 0:
            raise NRSC5Error("Failed to pipe samples.")
