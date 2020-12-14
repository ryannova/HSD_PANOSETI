/* HSD_compute_thread.c
 *
 * Does pre processing on the data coming from the quabos before writing to file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#include "hashpipe.h"
#include "HSD_databuf.h"

#define NUM_OF_MODES 7 // Number of mode and also used the create the size of array (Modes 1,2,3,6,7)

/**
 * Structure of the Quabo buffer stored for determining packet loss
 */
typedef struct quabo_info{
    uint16_t pkt_num[NUM_OF_MODES+1];
    uint16_t prev_pkt_num[NUM_OF_MODES+1];
    int lost_pkts[NUM_OF_MODES+1];
    quabo_info* next_quabo_info;
} quabo_info_t;

/**
 * Initializing a new quabo_info object
 */
quabo_info_t* quabo_info_t_new(){
    quabo_info_t* value = (quabo_info_t*) malloc(sizeof(struct quabo_info));
    memset(value->lost_pkts, -1, sizeof(value->lost_pkts));
    memset(value->pkt_num, 0, sizeof(value->pkt_num));
    memset(value->prev_pkt_num, 0, sizeof(value->prev_pkt_num));
    value->next_quabo_info = NULL;
    return value;
}

/**
 * Gets the quabo_info structure that corresponds to the quabo index
 * @param list The quabo object linked list to be incremented through
 * @param ind The quabo ind to find the appropriate object
 */ 
quabo_info_t* get_quabo_info(quabo_info_t* list, unsigned int ind){
    if(list != NULL && ind > 0)
        return get_quabo_info(list->next_quabo_info, ind-1);
    return list;
}

/**
 * Fliping the bytes of data1 and data2 to go from little-endian to big-endian
 */
uint16_t flipBytes(char data1, char data2){
    return ((data2 << 8) & 0xff00) | ((data1) & 0x00ff);
}

static void *run(hashpipe_thread_args_t * args){
    // Local aliases to shorten access to args fields
    HSD_input_databuf_t *db_in = (HSD_input_databuf_t *)args->ibuf;
    HSD_output_databuf_t *db_out = (HSD_output_databuf_t *)args->obuf;
    hashpipe_status_t st = args->st;
    const char * status_key = args->thread_desc->skey;

    // Index values for the circular buffers in the shared buffer with the input and output threads
    int rv;
    uint64_t mcnt=0;
    int curblock_in=0;
    int curblock_out=0;

    //Variables to display pkt info
    uint8_t mode;                                       //The current mode of the packet block
    quabo_info_t* quaboListBegin = quabo_info_t_new();  //Initializing the quabo info linked list
    quabo_info_t* quaboListEnd = quaboListBegin;        //Setting the pointer to be the end of the linked list
    unsigned int quaboListSize = 1;                     //Initalizing the size of the linked list
    unsigned int quaboInd[0xffff];                      //Create a rudimentary hash map of the quabo number and linked list ind
    memset(quaboInd, -1, sizeof(quaboInd));             //Set all of the values of the hash to be -1

    quabo_info_t* currentQuabo;                         //Pointer to the quabo info that is currently being used
    uint16_t boardLoc;                                  //The boardLoc(quabo index) for the current packet
    char* boardLocstr = (char *)malloc(sizeof(char)*10);


    /*uint16_t pkt_num[NUM_OF_MODES+1];
    uint16_t prev_pkt_num[NUM_OF_MODES+1];
    int lost_pkts[NUM_OF_MODES+1];
    memset(lost_pkts, -1, sizeof(lost_pkts));*/
    
    //Counters for the packets lost
    int total_lost_pkts = 0;
    int current_pkt_lost;

    //Compute Elements
    char *str_q;
    str_q = (char *)malloc(BLOCKSIZE*sizeof(unsigned char));

    printf("-----------Finished Setup of Compute Thread----------\n\n");

    
    while(run_threads()){
        hashpipe_status_lock_safe(&st);
        hputi4(st.buf, "COMBLKIN", curblock_in);
        hputs(st.buf, status_key, "waiting");
        hputi4(st.buf, "COMBKOUT", curblock_out);
	    hputi8(st.buf,"COMMCNT",mcnt);
        hashpipe_status_unlock_safe(&st);

        //Wait for new input block to be filled
        while ((rv=HSD_input_databuf_wait_filled(db_in, curblock_in)) != HASHPIPE_OK) {
            if (rv==HASHPIPE_TIMEOUT) {
                hashpipe_status_lock_safe(&st);
                hputs(st.buf, status_key, "blocked");
                hashpipe_status_unlock_safe(&st);
                continue;
            } else {
                hashpipe_error(__FUNCTION__, "error waiting for filled databuf");
                pthread_exit(NULL);
                break;
            }
        }

        // Wait for new output block to be free
        while ((rv=HSD_output_databuf_wait_free(db_out, curblock_out)) != HASHPIPE_OK) {
            if (rv==HASHPIPE_TIMEOUT) {
                hashpipe_status_lock_safe(&st);
                hputs(st.buf, status_key, "blocked compute out");
                hashpipe_status_unlock_safe(&st);
                continue;
            } else {
                hashpipe_error(__FUNCTION__, "error waiting for free databuf");
                pthread_exit(NULL);
                break;
            }
        }

        //Note processing status
        hashpipe_status_lock_safe(&st);
        hputs(st.buf, status_key, "processing packet");
        hashpipe_status_unlock_safe(&st);

        //CALCULATION BLOCK
        //TODO
        //Get data from buffer
        memcpy(str_q, db_in->block[curblock_in].data_block, BLOCKSIZE*sizeof(unsigned char));

        for(int i = 0; i < N_PKT_PER_BLOCK; i++){
            


            //Finding the packet number and computing the lost of packets by using packet number
            //Read the packet number from the packet
            mode = str_q[i*PKTSIZE];
            boardLoc = flipBytes(str_q[i*PKTSIZE+4], str_q[i*PKTSIZE+5]);
            #ifdef TEST_MODE
                //printf("Mode:%i ", mode);
                //printf("BoardLocation:%02X \n", boardLoc);
            #endif
            //Check to see if there is a quabo info for the current quabo packet. If not create an object
            if (quaboInd[boardLoc] == -1){
                quaboInd[boardLoc] = quaboListSize;                 //Setting the value to be the new element for quabo info
                quaboListSize++;                                    //Increase the size of the linked list by 1

                printf("New Quabo Detected ID:%u.%u\n", (boardLoc >> 8) & 0x00ff, boardLoc & 0x00ff); //Output the terminal the new quabo

                quaboListEnd->next_quabo_info = quabo_info_t_new(); //Create a new quabo info object
                quaboListEnd = quaboListEnd->next_quabo_info;       //Append the new quabo info to the end of the linked list
                currentQuabo = quaboListEnd;                        //Set the currentQuabo to point to the new quabo info
            } else {
                #ifdef TEST_MODE
                    //printf("Index:%i \n", quaboInd[boardLoc]);
                #endif
                currentQuabo = get_quabo_info(quaboListBegin, quaboInd[boardLoc]);
            }

            //Store the current quabo's mode
            currentQuabo->pkt_num[mode] = flipBytes(str_q[i*PKTSIZE+2], str_q[i*PKTSIZE+3]);

            #ifdef TEST_MODE
                printf("pkt_num:%i\n", pkt_num[mode]);
                printf("lost_pkt:%i\n\n", lost_pkts[mode]);
            #endif
            //Check to see if it is newly created quabo info if so then inialize the lost packet number to 0
            if (currentQuabo->lost_pkts[mode] < 0) {
                currentQuabo->lost_pkts[mode] = 0;
            } else {
                //Check to see if the current packet number is less than the previous. If so the number has overflowed and looped.
                //Compenstate for this if this has happend, and then take the difference of the packet numbers minus 1 to be the packets lost
                if (currentQuabo->pkt_num[mode] < currentQuabo->prev_pkt_num[mode])
                    current_pkt_lost = (0xffff - currentQuabo->prev_pkt_num[mode]) + currentQuabo->pkt_num[mode];
                else
                    current_pkt_lost = (currentQuabo->pkt_num[mode] - currentQuabo->prev_pkt_num[mode]) - 1;
                
                currentQuabo->lost_pkts[mode] += current_pkt_lost; //Add this packet lost to the total for this quabo
                total_lost_pkts += current_pkt_lost;               //Add this packet lost to the overall total for all quabos
            }
            currentQuabo->prev_pkt_num[mode] = currentQuabo->pkt_num[mode]; //Update the previous packet number to be the current packet number
        }

        //Copy the input packet to the output packet
        memcpy(db_out->block[curblock_out].result_block, str_q, BLOCKSIZE*sizeof(unsigned char));

        //Copy time over to output
        memcpy(db_out->block[curblock_out].header.tv_sec, db_in->block[curblock_in].header.tv_sec, N_PKT_PER_BLOCK*sizeof(long int));
        memcpy(db_out->block[curblock_out].header.tv_usec, db_in->block[curblock_in].header.tv_usec, N_PKT_PER_BLOCK*sizeof(long int));
        #ifdef TEST_MODE
            /*for (int i = 0; i < N_PKT_PER_BLOCK; i++){
                printf("TIME %li.%li\n", db_out->block[curblock_out].header.tv_sec[i], db_out->block[curblock_out].header.tv_usec[i]);
            }*/
        #endif
        /*Update input and output block for both buffers*/
        //Mark output block as full and advance
        HSD_output_databuf_set_filled(db_out, curblock_out);
        curblock_out = (curblock_out + 1) % db_out->header.n_block;

        //Mark input block as free and advance
        HSD_input_databuf_set_free(db_in, curblock_in);
        curblock_in = (curblock_in + 1) % db_in->header.n_block;
        mcnt++;

        sprintf(boardLocstr, "%u.%u", (boardLoc >> 8) & 0x00ff, boardLoc & 0x00ff);
        //display packetnum in status
        hashpipe_status_lock_safe(&st);
        hputs(st.buf, "QUABOKEY", boardLocstr);
        hputi4(st.buf, "M1PKTNUM", currentQuabo->pkt_num[1]);
        hputi4(st.buf, "M2PKTNUM", currentQuabo->pkt_num[2]);
        hputi4(st.buf, "M3PKTNUM", currentQuabo->pkt_num[3]);
        hputi4(st.buf, "M6PKTNUM", currentQuabo->pkt_num[6]);
        hputi4(st.buf, "M7PKTNUM", currentQuabo->pkt_num[7]);

        hputi4(st.buf, "TPKTLST", total_lost_pkts);
        hputi4(st.buf, "M1PKTLST", currentQuabo->lost_pkts[1]);
        hputi4(st.buf, "M2PKTLST", currentQuabo->lost_pkts[2]);
        hputi4(st.buf, "M3PKTLST", currentQuabo->lost_pkts[3]);
        hputi4(st.buf, "M6PKTLST", currentQuabo->lost_pkts[6]);
        hputi4(st.buf, "M7PKTLST", currentQuabo->lost_pkts[7]);
        hashpipe_status_unlock_safe(&st);

        //Check for cancel
        pthread_testcancel();

    }

    //printf("\n");
    return THREAD_OK;
}

/**
 * Sets the functions and buffers for this thread
 */
static hashpipe_thread_desc_t HSD_compute_thread = {
    name: "HSD_compute_thread",
    skey: "COMPUTESTAT",
    init: NULL,
    run: run,
    ibuf_desc: {HSD_input_databuf_create},
    obuf_desc: {HSD_output_databuf_create}
};

static __attribute__((constructor)) void ctor(){
    register_hashpipe_thread(&HSD_compute_thread);
}
