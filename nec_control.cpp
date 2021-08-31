#include <iostream>
#include <iomanip>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <vector>
#include <map>

#include "libs/CLI11.hpp"

typedef std::vector<uint8_t> nec_data_t;

enum nec_command_t { power, backlight };
enum nec_message_t {
    command             = 'A',
    command_reply       = 'B',
    get_parameter       = 'C',
    get_parameter_reply = 'D',
    set_parameter       = 'E',
    set_parameter_reply = 'F',
};

struct nec_request_t {
  nec_message_t msg_type;
  nec_data_t bytes;
};

static const size_t nec_max_msg_len = 64;   // Maximal receive buffer size
static const size_t nec_std_int_len = 4;    // Standard integer size (translated into hex string - 4char bytes - 2int bytes)
static const size_t nec_stx_etx_len = 2;    // STX and EXT size (2char bytes)
static const size_t nec_dsp_timeout = 2;    // Maximal time in seconds for display to answer

// Constants for commands described in the documentation
static const size_t nec_cmd_val_power_on  = 1; 
static const size_t nec_cmd_val_power_off = 4;

static const std::map<nec_command_t, nec_request_t> nec_commands_list = {
    {nec_command_t::power,     {nec_message_t::command,       {0x43, 0x32, 0x30, 0x33, 0x44, 0x36}}},
    {nec_command_t::backlight, {nec_message_t::set_parameter, {0x30, 0x30, 0x31, 0x30            }}},
};

int dsp_socket;

void connect(std::string remote_ip, std::string remote_port) {
    struct addrinfo * dsp_addrinfo;
    if (getaddrinfo(remote_ip.c_str(), remote_port.c_str(), NULL, &dsp_addrinfo)) {
        throw std::runtime_error("cannot get address info");
    }

    dsp_socket = socket(dsp_addrinfo->ai_family, SOCK_STREAM, 0);
    if (dsp_socket == -1) {
        freeaddrinfo(dsp_addrinfo);
        throw std::runtime_error("cannot create socket");
    }

    struct timeval dsp_timeout;
    dsp_timeout.tv_sec = nec_dsp_timeout;
    dsp_timeout.tv_usec = 0;
    setsockopt(dsp_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&dsp_timeout, sizeof(dsp_timeout));

    if (connect(dsp_socket, dsp_addrinfo->ai_addr, dsp_addrinfo->ai_addrlen) == -1) {
        ::close(dsp_socket);
        freeaddrinfo(dsp_addrinfo);
        throw std::runtime_error("cannot connect to monitor");
    }
    freeaddrinfo(dsp_addrinfo);
}

void disconnect() {
    ::close(dsp_socket);
}

void fill_buffer_start(nec_data_t &buffer, nec_message_t type, size_t len) {
    // Header
    if (len > 9) len += 7;      // Hex encoding (diff between ASCII '9' and 'A')
    buffer.push_back(0x01);     // Start of Header
    buffer.push_back(0x30);     // Reserved for future extensions.       '0'
    buffer.push_back(0x41);     // Destination equipment ID. (Receiver)  'A' == monitor ID 1
    buffer.push_back(0x30);     // Source equipment ID. (Sender)         '0' == controller is always zero
    buffer.push_back(type);     // Message Type: (Case sensitive.)       'A' == Command; 'B' == Command reply; 'C' ==  Get parameter; 'D' ==  Get parameter reply; 'E' == Set parameter; 'F' == Set parameter reply
    buffer.push_back(0x30);     // Message Length #1                     '0' == Byte enoded as a hex character, always zero, no longer messages than 15bytes
    buffer.push_back(0x30+len); // Message Length #2                     <L> == Byte enoded as a hex character
    // Start of message
    buffer.push_back(0x02);     // STX                                  0x02 == STX constant
}

void fill_buffer_end(nec_data_t &buffer) {
    // End of message
    buffer.push_back(0x03);     // ETX                                  0x03 == ETX constant
    // Compute Block Check Code
    uint8_t check = buffer[1];  // Skip start of header
    for (size_t index = 2; index < buffer.size(); index++) {
        check = check ^ buffer[index];
    }
    buffer.push_back(check);    // BCC
    // Delimiter - end of packet
    buffer.push_back(0x0D);
}

void fill_number(nec_data_t &buffer, uint16_t value) {
    std::stringstream s_number;
    s_number << std::setfill('0') << std::setw(4) << std::hex << value;
    buffer.push_back(s_number.str()[0]);
    buffer.push_back(s_number.str()[1]);
    buffer.push_back(s_number.str()[2]);
    buffer.push_back(s_number.str()[3]);
}

void check_answer() {
    nec_data_t buffer;
    buffer.resize(nec_max_msg_len);
    auto rcv_len = ::read(dsp_socket, &buffer[0], buffer.size());
    if (rcv_len < 0) {
        throw std::runtime_error("cannot read from socket");
    } else {
        // TODO: check the answer, this is a DEBUG output only!
        buffer.resize(rcv_len);
        for (auto &&byte : buffer) {
            std::cout << std::setfill('0') << std::setw(2) << std::hex << (int)byte << " ";
        }
        std::cout << std::endl;
    }
}

void send_standard_cmd(nec_command_t cmd, int value) {
    nec_data_t buffer;
    auto command = nec_commands_list.find(cmd)->second;
    fill_buffer_start(buffer, command.msg_type, command.bytes.size()+nec_std_int_len+nec_stx_etx_len);
    buffer.insert(buffer.end(), command.bytes.begin(), command.bytes.end());
    fill_number(buffer, value);
    fill_buffer_end(buffer);
    ::write(dsp_socket, &buffer[0], buffer.size());
    check_answer();
}

int main(int argc, char *argv[]) {
    CLI::App nc_app("NEC CONTROL");

    std::string remote_ip   = "10.0.0.240";
    std::string remote_port = "7142";
    std::string power_state;
    int backlight = -1;
    bool verbose = false;

    nc_app.add_option("-a,--address,address", remote_ip, "Address to connect to.");
    nc_app.add_option("--port", remote_port, "Port to connect to.");
    nc_app.add_option("-p,--power", power_state, "Set power to on or off.")->check(CLI::IsMember(std::set<std::string>({"on", "off"})));
    nc_app.add_option("-b,--backlight", backlight, "Set backlight to a specific value.")->check(CLI::Range(0,100));
    nc_app.add_flag("-v,--verbose", verbose, "Speak more to me.");

    CLI11_PARSE(nc_app, argc, argv);

	if (verbose) std::cout << "Connecting to IP " << remote_ip << ": ";

    try {
        connect(remote_ip, remote_port);
        if (verbose) std::cout << "connected." << std::endl;
        if (power_state == "on")  send_standard_cmd(nec_command_t::power, nec_cmd_val_power_on);
        if (power_state == "off") send_standard_cmd(nec_command_t::power, nec_cmd_val_power_off);
        if (backlight > 0) send_standard_cmd(nec_command_t::backlight, backlight);
        disconnect();
    } catch(const std::runtime_error& e) {
        std::cerr << "Not able to set the parameter: \"" << e.what() << "\"" << std::endl;
        return 1;
    }
    
    return 0;
}