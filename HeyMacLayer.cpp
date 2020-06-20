/* Copyright 2020 Dean Hall.  See LICENSE for details. */

#include <stdint.h>

#include "mbed.h"
#include "SX1276_LoRaRadio.h"

#include "AThread.h"
#include "HeyMacIdent.h"
#include "HeyMacLayer.h"
#include "HeyMacFrame.h"
#include "HeyMacCmd.h"


static int const THRD_STACK_SZ = 6 * 1024;
static uint32_t const THRD_PRDC_MS = 5000;


/** App-specific event flags */
enum
{
    EVT_RX_BEGIN = EVT_BASE_LAST << 1,
    EVT_RX_END = EVT_BASE_LAST << 2,
    EVT_TX_BEGIN = EVT_BASE_LAST << 3,
    EVT_TX_END = EVT_BASE_LAST << 4,

    EVT_BTN = EVT_BASE_LAST << 5, // For fun/debug (may change value)

    EVT_ALL = EVT_BASE_ALL
            | EVT_RX_BEGIN
            | EVT_RX_END
            | EVT_TX_BEGIN
            | EVT_TX_END
            | EVT_BTN    // For fun/debug
};


static Serial s_ser(USBTX, USBRX);


HeyMacLayer::HeyMacLayer(char const *cred_fn)
    : AThread(THRD_STACK_SZ, THRD_PRDC_MS)
{
    _hm_ident = new HeyMacIdent(cred_fn);
    _radio = new SX1276_LoRaRadio(
        HM_PIN_LORA_MOSI,
        HM_PIN_LORA_MISO,
        HM_PIN_LORA_SCK,
        HM_PIN_LORA_NSS,
        HM_PIN_LORA_RESET,
        HM_PIN_LORA_DIO0,
        HM_PIN_LORA_DIO1,
        HM_PIN_LORA_DIO2,
        HM_PIN_LORA_DIO3,
        HM_PIN_LORA_DIO4,
        HM_PIN_LORA_DIO5);
}

HeyMacLayer::~HeyMacLayer()
{
}


void HeyMacLayer::dump_regs(void)
{
    _radio->dump_regs();
}


void HeyMacLayer::evt_btn(void)
{
    _thread->flags_set(EVT_BTN);
}


void HeyMacLayer::_main(void)
{
    static uint16_t const BCN_PERIOD = 32000 / THRD_PRDC_MS;
    uint16_t bcn_cnt = BCN_PERIOD;
    uint32_t flags;
    radio_events_t mac_clbks;

    for (;;)
    {
        flags = ThisThread::flags_wait_any(EVT_ALL, true);

        if (flags & EVT_THRD_TERM)
        {
            break;
        }

        if (flags & EVT_THRD_INIT)
        {
            /* Parse in this thread with the large stack space */
            _hm_ident->parse_cred_file();

            /* Init the radio with these callbacks */
            mac_clbks.tx_done = mbed::callback(this, &HeyMacLayer::_tx_done_mac_clbk);
            mac_clbks.rx_done = mbed::callback(this, &HeyMacLayer::_rx_done_mac_clbk);
            mac_clbks.rx_error = mbed::callback(this, &HeyMacLayer::_rx_error_mac_clbk);
            mac_clbks.tx_timeout = mbed::callback(this, &HeyMacLayer::_tx_timeout_mac_clbk);
            mac_clbks.rx_timeout = mbed::callback(this, &HeyMacLayer::_rx_timeout_mac_clbk);
            mac_clbks.fhss_change_channel = mbed::callback(this, &HeyMacLayer::_fhss_change_channel_mac_clbk);
            mac_clbks.cad_done = mbed::callback(this, &HeyMacLayer::_cad_done_mac_clbk);
            _radio->init_radio(&mac_clbks);

            _radio->set_syncword(0x12);
            ThisThread::sleep_for(10);

            _radio->set_channel(432550000);
            ThisThread::sleep_for(10);

            _radio->set_tx_config(
                MODEM_LORA,
                12,         /*pwr*/
                0,          /*fdev (unused)*/
                1,          /*BW*/ // 1 +7 ==> 250 kHz
                7,          /*SF 128*/
                2,          /*CR 4/6*/
                8,          /*preamble*/
                false,      /*fixlen*/
                true,       /*crc*/
                false,      /*freqhop*/
                0,          /*hop prd*/
                false,      /*iq inverted*/
                275);       /*ms tx timeout*/
        }

        if (flags & EVT_THRD_PRDC)
        {
            if (--bcn_cnt == 0)
            {
                bcn_cnt = BCN_PERIOD;
                _tx_bcn();
            }
        }

        if (flags & EVT_RX_BEGIN)
        {
            // TODO: _radio->receive();
        }

        /* Transmit is done or timed-out and we resume continuous RX */
        if (flags & EVT_TX_END)
        {
            // TODO: flush, clean, change radio state?
            _thread->flags_set(EVT_RX_BEGIN);
        }

        if (flags & EVT_BTN)
        {
            // TEMPORARY: when the button is pressed, emit a HeyMac Txt Command with my callsign as the payload
            char tac_id[HM_IDENT_TAC_ID_SZ];
            HeyMacFrame frm;
            HeyMacCmd cmd;

            _hm_ident->copy_tac_id_into(tac_id);
            frm.set_protocol(HM_PIDFLD_CSMA_V0);
            frm.set_src_addr(_hm_ident->get_long_addr());
            cmd.cmd_init(&frm);
            cmd.cmd_txt(tac_id, strlen(tac_id));
            //TODO: enqueue (FIFO) frm instead of this:
            _radio->send(frm.get_ref(), frm.get_sz());
        }
    }
}


/* Callbacks for LoRaRadio */
void HeyMacLayer::_tx_done_mac_clbk(void)
{
    s_ser.puts("TXDONE\n");
    _thread->flags_set(EVT_TX_END);
}

void HeyMacLayer::_tx_timeout_mac_clbk(void)
{
    s_ser.puts("TXTO\n");
    _thread->flags_set(EVT_TX_END);
}

void HeyMacLayer::_rx_done_mac_clbk(uint8_t const *payload, uint16_t size, int16_t rssi, int8_t snr)
{
    s_ser.puts("RXDONE\n");
    _dump_buf(payload, size);
    _thread->flags_set(EVT_RX_END);
}

void HeyMacLayer::_rx_timeout_mac_clbk(void)
{
    s_ser.puts("RXTO\n");
    _thread->flags_set(EVT_RX_END);
}

void HeyMacLayer::_rx_error_mac_clbk(void)
{
    s_ser.puts("RXERR\n");
    _thread->flags_set(EVT_RX_END);
}

void HeyMacLayer::_fhss_change_channel_mac_clbk(uint8_t current_channel) {}
void HeyMacLayer::_cad_done_mac_clbk(bool channel_busy) {}


void HeyMacLayer::_tx_bcn(void)
{
    HeyMacFrame frm;
    HeyMacCmd cmd;

    frm.set_protocol(HM_PIDFLD_CSMA_V0);
    frm.set_src_addr(_hm_ident->get_long_addr());
    cmd.cmd_init(&frm);
    uint16_t const caps = 0xCA; // TODO: impl:
    uint16_t const status = 0x00; // status = red flags = (1==fault)
    cmd.cmd_cbcn(caps, status);   //TODO: , nets, ngbrs);

    //TODO: enqueue (LIFO) frm instead of this:
    _radio->send(frm.get_ref(), frm.get_sz());
}


void HeyMacLayer::_dump_buf(uint8_t const * const buf, uint8_t const sz)
{
    static char const hex[] = {"0123456789ABCDEF"};
    uint8_t ch;

    s_ser.putc('#');
    s_ser.putc(' ');
    for (int8_t i = 0; i < sz; i++ )
    {
        ch = buf[i];
        s_ser.putc(hex[ch >> 4]);
        s_ser.putc(hex[ch & 0x0F]);
        s_ser.putc(' ');
    }
    s_ser.putc('\n');
}
