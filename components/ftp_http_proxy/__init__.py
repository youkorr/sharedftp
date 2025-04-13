import esphome.codegen as cg
import esphome.config_validation as cv

CONF_ID = 'id'
CONF_SERVER = 'server'
CONF_USERNAME = 'username'
CONF_PASSWORD = 'password'
CONF_SHARED_PATH = 'shared_path'
CONF_LOCAL_PORT = 'local_port'

DEPENDENCIES = []
AUTO_LOAD = []

ftp_http_proxy_ns = cg.esphome_ns.namespace('ftp_http_proxy')
FTPHTTPProxy = ftp_http_proxy_ns.class_('FTPHTTPProxy', cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.Required(CONF_ID): cv.declare_id(FTPHTTPProxy),
    cv.Required(CONF_SERVER): cv.string,
    cv.Required(CONF_USERNAME): cv.string,
    cv.Required(CONF_PASSWORD): cv.string,
    cv.Required(CONF_SHARED_PATH): cv.string,
    cv.Optional(CONF_LOCAL_PORT, default=8000): cv.port,
})

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    
    cg.add(var.set_ftp_server(config[CONF_SERVER]))
    cg.add(var.set_username(config[CONF_USERNAME]))
    cg.add(var.set_password(config[CONF_PASSWORD]))
    cg.add(var.set_shared_path(config[CONF_SHARED_PATH]))
    cg.add(var.set_local_port(config[CONF_LOCAL_PORT]))
