#include "MergCanBus.h"

MergCanBus::MergCanBus()
{
    //ctor
    messageFilter=0;
    bufferIndex=0;
    //Can=MCP_CAN();
    memory=MergMemoryManagement();
    canMessage=CANMessage();
    nodeId=MergNodeIdentification();
    message=Message();
    skipMessage(RESERVED);
    softwareEnum=false;
    DEBUG=false;
}

MergCanBus::~MergCanBus()
{
    //dtor
}

bool MergCanBus::initCanBus(unsigned int port,unsigned int rate, int retries,unsigned int retryIntervalMilliseconds){

    unsigned int r=0;
    Can.set_cs(port);

    do {
        if (CAN_OK==Can.begin(rate)){
            return true;
        }
        r++;
        delay(retryIntervalMilliseconds);
    }while (r<retries);

   return false;
}

/*
Set the bit in the message bit filter
The messageFilter indicates if a message type will be handled or not
*/
void MergCanBus::setBitMessage(byte pos,bool val){
    if (val){
        bitSet(messageFilter,pos);
    }
    else{
        bitClear(messageFilter,pos);
    }
}

/*
Method that deals with the majority of messages and behavior. Auto enum, query requests
If a custom function is set it calls it for every non automatic message
*/
unsigned int MergCanBus::runAutomatic(){

    if (!readCanBus()){
        //nothing to do
        return NO_MESSAGE;
    }

    if (message.getRTR()){
        //if we are a device with can id
        //we need to answer this message
        if (nodeId.getNodeNumber()!=0){
            //create the response message with no data
            canMessage.clear();
            Can.sendMsgBuf(nodeId.getCanID(),0,0,canMessage.getData());
            return OK;
        }
    }

    //message for self enumeration
    if (message.getOpc()==OPC_ENUM && message.getNodeNumber()==nodeId.getNodeNumber()){
        doSelfEnnumeration(true);
        return OK;
    }

    //do self enumeration
    //collect the canid from messages with 0 size
    //the state can be a message or manually

    if (state_mode==SELF_ENUMERATION){
        unsigned long tdelay=millis()-timeDelay;

        if (tdelay>SELF_ENUM_TIME){
            finishSelfEnumeration();
        }

        if (canMessage.getCanMsgSize()==0){
            if (bufferIndex<TEMP_BUFFER_SIZE){
                buffer[bufferIndex]=canMessage.getCanId();
                bufferIndex++;
            }
        }
        return OK;
    }


    //treat each message individually to interpret the code
    switch (message.getType()){
        case (DCC):
            handleDCCMessages();
        break;
        case (ACCESSORY):
            handleACCMessages();
        break;
        case (GENERAL):
            handleGeneralMessages();
        break;
        case (CONFIG):
            if (message.getNodeNumber()==nodeId.getNodeNumber()){
                handleConfigMessages();
            }
        break;
        default:
            return UNKNOWN_MSG_TYPE;
    }
    return OK;

}

//read the can bus and load the data in canMessage
bool MergCanBus::readCanBus(){
    byte len=0;
    if(CAN_MSGAVAIL == Can.checkReceive()) // check if data coming
    {
        canMessage.clear();
        Can.readMsgBuf(&len, canMessage.getData());
        Can.getCanHeader(canMessage.get_header());
        if (Can.isRTMMessage()!=0){
            canMessage.setRTR();
        }
        canMessage.setCanMsgSize(len);
        return true;
    }
    return false;
}

//put node in setup mode
//changing from slim to flim
void MergCanBus::doSetup(){
    state_mode=SETUP;
    prepareMessage(OPC_RQNN);
    sendCanMessage(3);
}
//sent by a node when going out of service
void MergCanBus::doOutOfService(){
    prepareMessage(OPC_NNREL);
    sendCanMessage(3);
}

void MergCanBus::doSelfEnnumeration(bool softEnum){
    softwareEnum=softEnum;
    state_mode=SELF_ENUMERATION;
    Can.sendRTMMessage(nodeId.getCanID());
    timeDelay=millis();
}

void MergCanBus::finishSelfEnumeration(){
    state_mode=NORMAL;
    sortArray(buffer,bufferIndex);
    //run the buffer and find the lowest can_id
    byte cid=1;
    for (int i=0;i<bufferIndex;i++){
        if (cid<buffer[i]){
            break;
        }
        cid++;
    }
    if (cid>99){
        //send and error message
        if (softwareEnum){
            mergCanData[0]=OPC_CMDERR;
            mergCanData[1]=highByte(nodeId.getNodeNumber());
            mergCanData[2]=lowByte(nodeId.getNodeNumber());
            mergCanData[3]=7;
            Can.sendMsgBuf(nodeId.getCanID(),0,3,mergCanData);
        }
        return;
    }
    nodeId.setCanID(cid);
    memory.setCanId(cid);
    //TODO: check if it is from software

    if (softwareEnum){
        mergCanData[0]=OPC_NNACK;
        mergCanData[1]=highByte(nodeId.getNodeNumber());
        mergCanData[2]=lowByte(nodeId.getNodeNumber());
        Can.sendMsgBuf(nodeId.getCanID(),0,3,mergCanData);
    }

    return;
}



byte MergCanBus::handleConfigMessages(){

    //config messages should be directed to node number or device id
    byte ind;
    if (message.getNodeNumber()!=nodeId.getNodeNumber()) {
        if (state_mode!=SETUP){return OK;}
    }

    switch (message.getOpc()){
    case OPC_RSTAT:
        //command station
        return OK;
        break;
    case OPC_QNN:
        //response with a OPC_PNN if we have a node ID
        //[<MjPri><MinPri=3><CANID>]<B6><NN Hi><NN Lo><Manuf Id><Module Id><Flags>
        if (nodeId.getNodeNumber()>0){
            prepareMessage(OPC_PNN);
            return sendCanMessage(6);
        }
        break;
    case OPC_RQNP:
        //Answer with OPC_PARAMS
        //<0xEF><PARA 1><PARA 2><PARA 3> <PARA 4><PARA 5><PARA 6><PARA 7>
        //The parameters are defined as:
        //Para 1 The manufacturer ID as a HEX numeric (If the manufacturer has a NMRA
        //number this can be used)
        //Para 2 Minor code version as an alphabetic character (ASCII)
        //Para 3 Manufacturer�s module identifier as a HEX numeric
        //Para 4 Number of supported events as a HEX numeric
        //Para 5 Number of Event Variables per event as a HEX numeric
        //Para 6 Number of supported Node Variables as a HEX numeric
        //Para 7 Major version as a HEX numeric. (can be 0 if no major version allocated)
        //Para 8 Node Flags
        if (state_mode==SETUP){
            clearMsgToSend();
            prepareMessage(OPC_PARAMS);
            return sendCanMessage(8);
        }
        break;
    case OPC_RQNN:
        //Answer with OPC_NAME
        if (state_mode==SETUP){
            prepareMessage(OPC_NAME);
            return sendCanMessage(8);
        }
        break;

    case OPC_SNN:
        //set the node number
        //answer with OPC_NNACK
        if (state_mode==SETUP){
            nodeId.setNodeNumber(message.getNodeNumber());
            //[TODO:save the data in memory]
            memory.setCanId(message.getNodeNumber());
            prepareMessage(OPC_NNACK);
            state_mode=NORMAL;
            return sendCanMessage(3);
        }
        break;
    case OPC_NNLRN:
        state_mode=LEARN;
        break;

    case OPC_NNULN:
        state_mode=NORMAL;
        break;

    case OPC_NNCLR:
        //clear all events from the node
        if (state_mode==LEARN){
            memory.eraseAllEvents();
            return OK;
        }
        break;
    case OPC_NNEVN:
        prepareMessage(OPC_EVNLF);
        return sendCanMessage(4);
        break;

    case OPC_NERD:
        //send back all stored events in message OPC_ENRSP
        int i;
        i=(int)memory.getNumEvents();
        if (i>0){
            byte *events=memory.getEvents();
            int pos=0;
            for (int j=0;j<i;j++){
                mergCanData[0]=OPC_ENRSP;
                mergCanData[1]=highByte(nodeId.getNodeNumber());
                mergCanData[2]=lowByte(nodeId.getNodeNumber());
                mergCanData[3]=events[pos];pos++;
                mergCanData[4]=events[pos];pos++;
                mergCanData[5]=events[pos];pos++;
                mergCanData[6]=events[pos];pos++;
                mergCanData[7]=j;
                ind=sendCanMessage(8);
            }
        }
        break;

    case OPC_RQEVN:
        prepareMessage(OPC_NUMEV);
        sendCanMessage(4);
        break;
    case OPC_BOOT:
        return OK;
        break;
    case OPC_ENUM:
        //has to be handled in the automatic procedure
        doSelfEnnumeration(true);
        break;
    case OPC_NVRD:
        //answer with NVANS
        ind=message.getNodeVariableIndex();
        clearMsgToSend();
        mergCanData[0]=OPC_NVANS;
        mergCanData[1]=highByte(nodeId.getNodeNumber());
        mergCanData[2]=lowByte(nodeId.getNodeNumber());
        mergCanData[3]=ind;
        mergCanData[4]=memory.getVar(ind);
        sendCanMessage(5);
        break;

    case OPC_NENRD:
        clearMsgToSend();
        ind=message.getEventIndex();
        byte *event;
        event=memory.getEvent(ind);
        mergCanData[0]=OPC_ENRSP;
        mergCanData[1]=highByte(nodeId.getNodeNumber());
        mergCanData[2]=lowByte(nodeId.getNodeNumber());
        mergCanData[3]=event[0];
        mergCanData[4]=event[1];
        mergCanData[5]=event[2];
        mergCanData[6]=event[3];
        mergCanData[7]=ind;
        break;

    case OPC_RQNPN:
        //answer with PARAN
        clearMsgToSend();
        ind=message.getParaIndex();
        mergCanData[0]=OPC_PARAN;
        mergCanData[1]=highByte(nodeId.getNodeNumber());
        mergCanData[2]=lowByte(nodeId.getNodeNumber());
        mergCanData[3]=ind;
        mergCanData[4]=nodeId.getParameter(ind);
        sendCanMessage(5);
        break;
    case OPC_CANID:
        ind=message.getData()[3];
        nodeId.setCanID(ind);
        memory.setCanId(ind);
        prepareMessage(OPC_NNACK);
        sendCanMessage(3);
        break;
    case OPC_EVULN:
        //[TODO]
        break;
    }
    return OK;
}

void MergCanBus::sortArray(byte *a, byte n){

  for (byte i = 1; i < n; ++i)
  {
    byte j = a[i];
    byte k;
    for (k = i - 1; (k >= 0) && (j < a[k]); k--)
    {
      a[k + 1] = a[k];
    }
    a[k + 1] = j;
  }
}

void MergCanBus::clearMsgToSend(){
    for (int i=0;i<CANDATA_SIZE;i++){
        mergCanData[i]=0;
    }
}

byte MergCanBus::sendCanMessage(byte message_size){
    byte r=Can.sendMsgBuf(nodeId.getCanID(),0,message_size,mergCanData);
    if (CAN_OK!=r){
        return r;
    }
    return OK;
}

void MergCanBus::setDebug(bool debug){
    DEBUG=debug;
}

void MergCanBus::prepareMessage(byte opc){

    clearMsgToSend();
    switch (opc){
    case OPC_PNN:
        mergCanData[0]=OPC_PNN;
        mergCanData[1]=highByte(nodeId.getNodeNumber());
        mergCanData[2]=lowByte(nodeId.getNodeNumber());
        mergCanData[3]=nodeId.getManufacturerId();
        mergCanData[4]=nodeId.getModuleId();
        mergCanData[5]=nodeId.getFlags();
        break;
    case OPC_NAME:
        mergCanData[0]=OPC_NAME;
        mergCanData[1]=nodeId.getNodeName()[0];
        mergCanData[2]=nodeId.getNodeName()[1];
        mergCanData[3]=nodeId.getNodeName()[2];
        mergCanData[4]=nodeId.getNodeName()[3];
        mergCanData[5]=nodeId.getNodeName()[4];
        mergCanData[6]=nodeId.getNodeName()[5];
        mergCanData[7]=nodeId.getNodeName()[6];
        break;
    case OPC_PARAMS:
        mergCanData[0]=OPC_PARAMS;
        mergCanData[1]=nodeId.getManufacturerId();
        mergCanData[2]=nodeId.getMinCodeVersion();
        mergCanData[3]=nodeId.getSuportedEvents();
        mergCanData[4]=nodeId.getSuportedEventsVariables();
        mergCanData[5]=nodeId.getSuportedNodeVariables();
        mergCanData[6]=nodeId.getMaxCodeVersion();
        mergCanData[7]=nodeId.getFlags();
        break;
    case OPC_NNACK:
        mergCanData[0]=OPC_NNACK;
        mergCanData[1]=highByte(nodeId.getNodeNumber());
        mergCanData[2]=lowByte(nodeId.getNodeNumber());
        break;

    case OPC_NNREL:
        mergCanData[0]=OPC_NNREL;
        mergCanData[1]=highByte(nodeId.getNodeNumber());
        mergCanData[2]=lowByte(nodeId.getNodeNumber());
        break;
    case OPC_EVNLF:
        mergCanData[0]=OPC_EVNLF;
        mergCanData[1]=highByte(nodeId.getNodeNumber());
        mergCanData[2]=lowByte(nodeId.getNodeNumber());
        mergCanData[3]=nodeId.getSuportedEvents()-memory.getNumEvents();
        break;
    case OPC_NUMEV:
        mergCanData[0]=OPC_NUMEV;
        mergCanData[1]=highByte(nodeId.getNodeNumber());
        mergCanData[2]=lowByte(nodeId.getNodeNumber());
        mergCanData[3]=memory.getNumEvents();
        break;
    case OPC_WRACK:
        mergCanData[0]=OPC_WRACK;
        mergCanData[1]=highByte(nodeId.getNodeNumber());
        mergCanData[2]=lowByte(nodeId.getNodeNumber());
        break;
    }
}

