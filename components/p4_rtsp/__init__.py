import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button, microphone, speaker
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.const import CONF_ID

DEPENDENCIES = ["network"]

AUTO_LOAD = ["button"]

CONF_PORT = "port"
CONF_VIDEO = "video"
CONF_RESOLUTION = "resolution"
CONF_FPS = "fps"
CONF_BITRATE = "bitrate"
CONF_GOP = "gop"
CONF_DATA_LANES = "data_lanes"
CONF_SCCB_SDA = "sccb_sda"
CONF_SCCB_SCL = "sccb_scl"
CONF_XCLK_PIN = "xclk_pin"
CONF_POWER_DOWN_PIN = "power_down_pin"
CONF_ALWAYS_ON = "always_on"
CONF_AUDIO = "audio"
CONF_MICROPHONE = "microphone"
CONF_SPEAKER = "speaker"
CONF_SAMPLE_RATE = "sample_rate"
CONF_CHANNELS = "channels"
CONF_TEST_TONE = "test_tone"

p4_rtsp_ns = cg.esphome_ns.namespace("p4_rtsp")
P4RtspStream = p4_rtsp_ns.class_("P4RtspStream", cg.Component)
TestToneButton = p4_rtsp_ns.class_("TestToneButton", button.Button, cg.Component)

RESOLUTIONS = {
    "320x240": (320, 240),
    "640x480": (640, 480),
    "800x800": (800, 800),
    "800x1280": (800, 1280),
    "1280x960": (1280, 960),
    "1280x720": (1280, 720),
    "1920x1080": (1920, 1080),
}

VIDEO_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_RESOLUTION, default="1280x720"): cv.enum(RESOLUTIONS, lower=True),
        cv.Optional(CONF_FPS, default=25): cv.int_range(min=1, max=60),
        cv.Optional(CONF_BITRATE, default=4000000): cv.int_range(min=100000, max=20000000),
        cv.Optional(CONF_GOP, default=25): cv.int_range(min=1, max=250),
        cv.Optional(CONF_DATA_LANES, default=2): cv.one_of(1, 2),
        cv.Optional(CONF_SCCB_SDA, default=7): cv.int_,
        cv.Optional(CONF_SCCB_SCL, default=8): cv.int_,
        cv.Optional(CONF_XCLK_PIN, default=40): cv.int_,
        cv.Optional(CONF_POWER_DOWN_PIN, default=-1): cv.int_,
        cv.Optional(CONF_ALWAYS_ON, default=False): cv.boolean,
    }
)

AUDIO_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Optional(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(min=8000, max=48000),
        cv.Optional(CONF_CHANNELS, default=1): cv.one_of(1, 2),
        cv.Optional(CONF_TEST_TONE): button.button_schema(TestToneButton),
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(P4RtspStream),
            cv.Optional(CONF_PORT, default=554): cv.int_range(min=1, max=65535),
            cv.Optional(CONF_VIDEO): VIDEO_SCHEMA,
            cv.Optional(CONF_AUDIO): AUDIO_SCHEMA,
        }
    ),
    cv.has_at_least_one_key(CONF_VIDEO, CONF_AUDIO),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_port(config[CONF_PORT]))

    if CONF_VIDEO in config:
        video = config[CONF_VIDEO]
        width, height = video[CONF_RESOLUTION].enum_value
        cg.add(var.set_video_enabled(True))
        cg.add(var.set_video_resolution(width, height))
        cg.add(var.set_video_fps(video[CONF_FPS]))
        cg.add(var.set_video_bitrate(video[CONF_BITRATE]))
        cg.add(var.set_video_gop(video[CONF_GOP]))
        cg.add(var.set_camera_pins(video[CONF_SCCB_SDA], video[CONF_SCCB_SCL],
                                   video[CONF_XCLK_PIN], video[CONF_DATA_LANES]))
        cg.add(var.set_camera_power_down_pin(video[CONF_POWER_DOWN_PIN]))
        cg.add(var.set_video_always_on(video[CONF_ALWAYS_ON]))

        # Enable the OV5647 MIPI-CSI sensor driver and pick its default output
        # format so it matches the requested resolution as closely as possible.
        add_idf_sdkconfig_option("CONFIG_CAMERA_OV5647", True)
        # Keep the ISP control path and MIPI-CSI ISR out of flash/cache to avoid
        # ISP FIFO overflows under sustained capture load.
        add_idf_sdkconfig_option("CONFIG_ISP_CTRL_FUNC_IN_IRAM", True)
        add_idf_sdkconfig_option("CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE", True)
        add_idf_sdkconfig_option("CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER", True)
        ov5647_fmt = {
            (320, 240): "CONFIG_CAMERA_OV5647_MIPI_RAW8_800x640_50FPS",
            (640, 480): "CONFIG_CAMERA_OV5647_MIPI_RAW8_800x640_50FPS",
            (800, 1280): "CONFIG_CAMERA_OV5647_MIPI_RAW8_800x1280_50FPS",
            (800, 800): "CONFIG_CAMERA_OV5647_MIPI_RAW8_800x800_50FPS",
            (1280, 960): "CONFIG_CAMERA_OV5647_MIPI_RAW10_1280x960_BINNING_45FPS",
        }
        ov5647_fmt = ov5647_fmt.get((width, height), "CONFIG_CAMERA_OV5647_MIPI_RAW10_1920x1080_30FPS")
        add_idf_sdkconfig_option(ov5647_fmt, True)

    if CONF_AUDIO in config:
        audio = config[CONF_AUDIO]
        cg.add(var.set_audio_sample_rate(audio[CONF_SAMPLE_RATE]))
        cg.add(var.set_audio_channels(audio[CONF_CHANNELS]))
        if CONF_MICROPHONE in audio:
            mic = await cg.get_variable(audio[CONF_MICROPHONE])
            cg.add(var.set_microphone(mic))
        if CONF_SPEAKER in audio:
            spk = await cg.get_variable(audio[CONF_SPEAKER])
            cg.add(var.set_speaker(spk))
        if CONF_TEST_TONE in audio:
            if CONF_SPEAKER not in audio:
                raise cv.Invalid(
                    f"'{CONF_TEST_TONE}' requires the '{CONF_SPEAKER}' option in the audio section"
                )
            tone_cfg = audio[CONF_TEST_TONE]
            tone = cg.new_Pvariable(tone_cfg[CONF_ID])
            cg.add(tone.set_stream(var))
            await button.register_button(tone, tone_cfg)
