/**
 * DISCLAIMER
 * This is our "functional prototype", this means that even though
 * it is kinda functional, there are plenty of security holes and bugs.
 * That's why you are not able to store your private keys here - 
 * only public information. And you should NOT trust this wallet
 * Use it carefully, on the testnet, otherwise you could lose your funds.
 *
 * Also architecture and the whole codebase will be refactored significantly
 * in the future and we are not maintaining backwards compatibility.
 */

#include "mbed.h"

#include <stdio.h>
#include <stdlib.h>

#include "specter_config.h"
#include "helpers.h"
#include "storage.h"
#include "gui.h"
#include "rng.h"
#include "host.h"
#include "keystore.h"
#include "networks.h"

#include "wally_core.h"
#include "wally_bip39.h"
#include "wally_address.h"
 
#include "wally_psbt.h"
#include "wally_script.h"

#define NO_ACTION       0
#define VERIFY_ADDRESS  1
#define SIGN_PSBT       2

static char * mnemonic = NULL;
static char * password = NULL;

static keystore_t keystore;
static const network_t * network = &Testnet;

static int in_action = NO_ACTION;
static struct wally_psbt * psbt = NULL;

Serial pc(SERIAL_TX, SERIAL_RX, 115200);
DigitalIn btn(USER_BUTTON);

// generates a mnemonic from entropy
// TODO: should it be moved to keystore?
void generate_mnemonic(size_t n){
    uint8_t * rnd;
    rnd = (uint8_t *) malloc(n);
    rng_get_random_buffer(rnd, n);
    bip39_mnemonic_from_bytes(NULL, rnd, n, &mnemonic);
    wally_bzero(rnd, n);
    free(rnd);
}

// securely copies the string and zeroes input
void sstrcopy(char * input, char ** output){
    if(*output!=NULL){
        wally_bzero(*output, strlen(*output));
        free(*output);
        *output = NULL;
    }
    *output = (char*)calloc(strlen(input)+1, sizeof(char));
    strcpy(*output, input);
    wally_bzero(input, strlen(input));
}

// initializes keystore from mnemonic and password
void init_keys(const char * mnemonic, const char * password, keystore_t * keys){
    logit("main", "init_keys");
    keystore_init(mnemonic, password, keys);
}

// sets default extended keys paths in the GUI
void set_default_xpubs(){
    // set default xpubs derivations
    char single[20];
    char multisig[20];
    sprintf(single, "m/84h/%luh/0h", network->bip32);
    sprintf(multisig, "m/48h/%luh/0h/2h", network->bip32);
    gui_set_default_xpubs(single, multisig);
}

// parses psbt, constructs all the addresses and amounts and sends to GUI
static int show_psbt(const struct wally_psbt * psbt){
    // check if we can sign it and all fields are ok
    int res = keystore_check_psbt(&keystore, psbt);
    if(res!=0){
        if(res & KEYSTORE_PSBTERR_CANNOT_SIGN){
            show_err("Can't sign the transaction");
            return -1;
        }
        if(res & KEYSTORE_PSBTERR_MIXED_INPUTS){
            show_err("Mixed inputs are not supported yet");
            return -1;
        }
        if(res & KEYSTORE_PSBTERR_WRONG_FIELDS){
            show_err("Something is wrong with transaction fields");
            return -1;
        }
        if(res & KEYSTORE_PSBTERR_UNSUPPORTED_POLICY){
            show_err("Script policy is not supported");
            return -1;
        }
        show_err("Something is wrong with transaction");
            return -1;
    }

    uint64_t in_amount = 0;
    uint64_t out_amount = 0;
    uint64_t change_amount = 0;
    uint64_t fee = 0;

    for(int i = 0; i < psbt->num_inputs; i++){
        if(!psbt->inputs[i].witness_utxo){
            show_err("Unsupported legacy transaction or missing prevout information");
            return -1;
        }
        in_amount += psbt->inputs[i].witness_utxo->satoshi;
    }

    txout_t * outputs;
    outputs = (txout_t *)calloc(psbt->num_outputs, sizeof(txout_t));

    for(int i=0; i < psbt->tx->num_outputs; i++){
        size_t script_type;
        wally_scriptpubkey_get_type(psbt->tx->outputs[i].script, psbt->tx->outputs[i].script_len, &script_type);
        char * addr = NULL;
        uint8_t bytes[21];
        // should deal with all script types, only P2WPKH for now
        switch(script_type){
            case WALLY_SCRIPT_TYPE_P2WPKH:
            case WALLY_SCRIPT_TYPE_P2WSH:
                wally_addr_segwit_from_bytes(psbt->tx->outputs[i].script, psbt->tx->outputs[i].script_len, network->bech32, 0, &addr);
                break;
            case WALLY_SCRIPT_TYPE_P2SH:
                bytes[0] = network->p2sh;
                memcpy(bytes+1, psbt->tx->outputs[i].script+2, 20);
                wally_base58_from_bytes(bytes, 21, BASE58_FLAG_CHECKSUM, &addr);
                break;
            case WALLY_SCRIPT_TYPE_P2PKH:
                bytes[0] = network->p2pkh;
                memcpy(bytes+1, psbt->tx->outputs[i].script+3, 20);
                wally_base58_from_bytes(bytes, 21, BASE58_FLAG_CHECKSUM, &addr);
                break;
        }
        if(!addr){
            addr = (char *)malloc(20);
            sprintf(addr, "...custom script...");
        }
        outputs[i].address = addr;
        outputs[i].amount = psbt->tx->outputs[i].satoshi;
        char * warning = NULL;
        outputs[i].is_change = keystore_output_is_change(&keystore, psbt, i, &warning);
        outputs[i].warning = warning;

        out_amount += psbt->tx->outputs[i].satoshi;
    }
    fee = in_amount-out_amount;
    gui_show_psbt(out_amount, change_amount, fee, psbt->num_outputs, outputs);
    for(int i=0; i<psbt->num_outputs; i++){
        if(outputs[i].address != NULL){
            wally_free_string(outputs[i].address);
        }
        if(outputs[i].warning != NULL){
            wally_free_string(outputs[i].warning);
        }
    }
    free(outputs);
    return 0;
}

// handles user action from GUI
void process_action(int action){
    switch(action){
        case GUI_SECURE_SHUTDOWN:
        {   
            logit("main", "shutting down...");
            char * s = gui_get_str();
            wally_bzero(s, strlen(s));
            wally_cleanup(0);
            exit(0);
            break; // no need really
        }
        case GUI_GENERATE_KEY: 
        {
            logit("main", "generating a key...");
            int v = gui_get_value();
            if(v % 3 != 0 || v < 12 || v > 24){
                v = SPECTER_MNEMONIC_WORDS;
            }
            generate_mnemonic(v*16/12);
            gui_show_mnemonic(mnemonic);
            break;
        }
        case GUI_PROCESS_MNEMONIC:
        {
            logit("main", "processing mnemonic...");
            char * str = gui_get_str();
            if(bip39_mnemonic_validate(NULL, str)!=WALLY_OK){
                show_err("mnemonic is not correct");
            }else{
                sstrcopy(str, &mnemonic);
                logit("main", "mnemonic is saved in memory");
                gui_get_password();
            }
            break;
        }
        case GUI_PROCESS_PASSWORD:
        {
            logit("main", "processing password");
            char * str = gui_get_str();
            sstrcopy(str, &password);
            logit("main", "password is saved in memory");
            init_keys(mnemonic, password, &keystore);
            // delete password from memory - we don't need it anymore
            wally_bzero(password, strlen(password));
            free(password);
            password = NULL;
            
            gui_show_main_screen();
            break;
        }
        case GUI_PROCESS_NETWORK:
        {
            int val = gui_get_value();
            if(val >= 0 && val < NETWORKS_NUM){
                network = networks[val];
                gui_set_network(val);
                set_default_xpubs();
                gui_show_main_screen();
            }else{
                show_err("No such network");
            }
            break;
        }
        case GUI_SHOW_XPUB:
        {
            char * str = gui_get_str();
            char *xpub = NULL;
            keystore_get_xpub(&keystore, str, network, &xpub); // keys, derivation, network, string
            gui_show_xpub(keystore.fingerprint, str, xpub);    //[fingerprint/derivation]xpub
            wally_free_string(xpub);
            break;
        }
        case GUI_VERIFY_ADDRESS:
        {
            logit("main", "verify address triggered");
            host_request_data();
            in_action = VERIFY_ADDRESS;
            break;
        }
        case GUI_SIGN_PSBT:
        {
            logit("main", "PSBT triggered");
            host_request_data();
            in_action = SIGN_PSBT;
            break;
        }
        case GUI_PSBT_CONFIRMED:
        {
            logit("main", "Signing transaction...");
            char * output = NULL;
            if(keystore_sign_psbt(&keystore, psbt, &output) == 0){
                printf("%s\r\n", output);
                gui_show_signed_psbt(output);
                wally_free_string(output);
            }else{
                show_err("failed to sign transaction");
            }
            break;
        }
        case GUI_BACK:
        {
            gui_show_init_screen();
            break;
        }
        default:
            show_err("unrecognized action");
    }
}

// handles data from the host
static void process_data(int action, uint8_t * buf, size_t len){
    int err;
    string derivation;
    char * b64 = NULL;
    switch(action){
        case VERIFY_ADDRESS:
        {
            // TODO: refactor to support multisig and bitcoin:addr?index=X codes
            char address[100];
            char type[10];
            char der[100];
            int res = sscanf((char *)buf, "address=%s\ntype=%s\n%s", address, type, der);
            if(res > 0){ // if "address=<addr>\ntype=<type>\n<der>" format is used
                if(memcmp(der, keystore.fingerprint, 8) != 0){
                    show_err("Wrong fingerprint");
                    return;
                }
                derivation = string(der+9)+string(address+1);
            }else{
                if(memcmp(buf, "m/", 2)!=0){
                    // check that fingerprint is ok
                    if(memcmp(buf, keystore.fingerprint, 8) != 0){
                        show_err("Wrong fingerprint");
                        return;
                    }
                    derivation = ((char *)buf + 9);
                }else{
                    derivation = (char *) buf;
                }
            }
            char * bech32_addr;
            err = keystore_get_addr(&keystore, derivation.c_str(), network, &bech32_addr, KEYSTORE_BECH32_ADDRESS);
            if(err){
                show_err("failed to derive address");
                return;
            }
            char * base58_addr;
            err = keystore_get_addr(&keystore, derivation.c_str(), network, &base58_addr, KEYSTORE_BASE58_ADDRESS);
            if(err){
                show_err("failed to derive address");
                return;
            }
            gui_show_addresses((char *)buf, bech32_addr, base58_addr);
            wally_free_string(bech32_addr);
            wally_free_string(base58_addr);
            break;
        }
        case SIGN_PSBT:
        {
            b64 = (char *) buf;
            if(psbt!=NULL){
                wally_psbt_free(psbt);
                psbt = NULL;
            }
            err = wally_psbt_from_base64(b64, &psbt);
            if(err!=WALLY_OK){
                show_err("failed to parse psbt transaction");
                return;
            }
            err = show_psbt(psbt);
            if(err){
                wally_psbt_free(psbt);
                psbt = NULL;
            }
            break;
        }
    }
}

void update(){
    gui_update();
    int action = gui_get_action();
    if(action != GUI_NO_ACTION){
        process_action(action);
        gui_clear_action();
    }
    host_update();
    if(in_action != NO_ACTION){
        size_t len = host_data_available();
        if(len > 0){
            logit("main", "data!");
            uint8_t * buf = host_get_data();
            process_data(in_action, buf, len);
            host_flush();
            in_action = NO_ACTION;
        }
    }

    if(btn){ // If blue button is pressed - calibrate touchscreen
        while(btn){
            wait(0.1);
        }
        gui_calibrate();
    }
}

int main(){
    int err = 0;

    rng_init();                     // random number generator
    storage_init();                 // on-board memory & sd card
                                    // on-board memory is on external chip => untrusted
    host_init(HOST_DEFAULT, 5);     // communication - functions to scan qr codes 
                                    //                 and talk to sd card storage
    wally_init(0);                  // init wally library
    // key storage module - signs, derives addresses etc.
    // if mnemonic and password are NULL just allocates space for the key
    keystore_init(NULL, NULL, &keystore); 

    gui_init();                     // display functions

    // available networks
    static const char * available_networks[] = {"Mainnet", "Testnet", "Regtest", "Signet", ""};
    gui_set_available_networks(available_networks);
    gui_set_network(1);             // default network - testnet
    set_default_xpubs();            // sets default xpub derivations

    // for debug purposes - hardcoded mnemonic
    // TODO: add reckless storage option
#ifdef DEBUG_MNEMONIC
    char debug_mnemonic[] = DEBUG_MNEMONIC;
    sstrcopy(debug_mnemonic, &mnemonic);
    gui_get_password();             // go directly to "enter password" screen
#else
    gui_start();                    // start the gui
#endif

    while(1){
        update();
    }
    // should never reach this, but still
    wally_cleanup(0);
    return err;
}