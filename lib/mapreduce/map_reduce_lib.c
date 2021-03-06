/*
 * map_reduce_lib.c
 *
 *      Author: yaroslav
 */

#include <stdlib.h> //calloc
#include <stdio.h>  //puts
#include <string.h> //memcpy
#include <unistd.h> //ssize_t
#include <sys/types.h> //temp read file
#include <sys/stat.h> //temp read file
#include <fcntl.h> //temp read file
#include <assert.h> //assert
#include <alloca.h>

#include "map_reduce_lib.h"
#include "mr_defines.h"
#include "elastic_mr_item.h"
#include "eachtoother_comm.h"
#include "channels_conf.h"
#include "buffered_io.h"

#include "buffer.h"

static size_t 
MapInputDataProvider( int fd, 
		      char **input_buffer, 
		      size_t requested_buf_size, 
		      int unhandled_data_pos ){
    WRITE_FMT_LOG("MapInputDataProvider *input_buffer=%p, fd=%d, "
		  "requested_buf_size=%u, unhandled_data_pos=%d\n",
		  *input_buffer, fd, (uint32_t)requested_buf_size, unhandled_data_pos );
    size_t rest_data_in_buffer = requested_buf_size - unhandled_data_pos;
    int first_call = *input_buffer == NULL ? 1 : 0;
    if ( !*input_buffer ){
	/*if input buffer not yet initialized then alloc it. 
	  It should be deleted by our framework after use*/
	*input_buffer = calloc( requested_buf_size+1, sizeof(char) );
	/*no data in buffer, it seems to be a first call of DataProvider*/
	rest_data_in_buffer = 0;
    }

    /**********************************************************************
     * Check for unsupported data handling. User should get handled data pos,
     * and we should concatenate unprocessed data with new data from input file.
     * For first launch of DataProvider unhandled_data_pos is ignoring*/
    if ( !first_call ){
	if ( !unhandled_data_pos ){
	    /* user Map function not hadnled data at all. In any case user Map must
	     * handle data, but in case if insufficiently of data in input buffer
	     * then user should increase buffer size via MAP_CHUNK_SIZE*/
	    assert( unhandled_data_pos );
	}
	else if ( unhandled_data_pos < requested_buf_size/2 ){
	    /* user Map function hadnled less than half of data. In normal case we need
	     * to move the rest of data to begining of buffer, but we can't do it just
	     * move because end of moved data and start of unhandled data is overlaps;
	     * For a while we don't want create another heap buffers to join rest data 
	     * and new data; */
	    assert( unhandled_data_pos > requested_buf_size/2 );
	}
	else if ( unhandled_data_pos != requested_buf_size )
	    {
		/*if not all data processed by previous call of DataProvider 
		 *then copy the rest into input_buffer*/
		memcpy( (*input_buffer), 
			(*input_buffer)+unhandled_data_pos, 
			rest_data_in_buffer );
	    }
    }
    ssize_t readed = read( fd, 
			   *input_buffer, 
			   requested_buf_size - rest_data_in_buffer );

    WRITE_FMT_LOG("MapInputDataProvider OK return handled bytes=%u\n", 
		  (uint32_t)(rest_data_in_buffer+readed) );
    return rest_data_in_buffer+readed;
}


void 
LocalSort( struct MapReduceUserIf *mif, Buffer *sortable ){
    /*sort created array*/
    qsort( sortable->data, 
	   sortable->header.count, 
	   sortable->header.item_size, 
	   mif->ComparatorMrItem );
}


size_t
GetHistogram( struct MapReduceUserIf *mif,
	      const Buffer* map, 
	      int step, 
	      Histogram *histogram ){
    /*access hash keys in map buffer and add every N hash into histogram*/
    const ElasticBufItemData* mritem;

    if ( !map->header.count ) return 0;
    if ( !step ){
	mritem = (const ElasticBufItemData*) BufferItemPointer(map, map->header.count-1);
	AddBufferItem( &histogram->buffer, &mritem->key_hash );
	histogram->step_hist_common = histogram->step_hist_last = map->header.count;
    }
    else{
	histogram->step_hist_common = MIN( step, map->header.count );
	int i = -1; /*array index*/
	do{
	    histogram->step_hist_last = MIN( step, map->header.count-i-1 );
	    i += histogram->step_hist_last;
	    mritem = (const ElasticBufItemData*) BufferItemPointer(map, i);
	    AddBufferItem( &histogram->buffer, &mritem->key_hash );
	}while( i < map->header.count-1 );

    }
#ifdef DEBUG
    if ( mif && mif->DebugHashAsString ){
	WRITE_FMT_LOG("\nstep=%d/%d, Histogram=[ ", 
		      histogram->step_hist_common, histogram->step_hist_last);
	uint8_t* hash;
	int i;
	for( i=0; i < histogram->buffer.header.count ; i++ ){
	    hash = (uint8_t*) BufferItemPointer( &histogram->buffer, i);
	    WRITE_FMT_LOG("%s ", PRINTABLE_HASH(mif, hash) );
	}
	WRITE_LOG("]\n"); fflush(0);
    }
#endif
    return histogram->buffer.header.count;
}

/************************************************************************
 * EachToOtherPattern callback.
 * Every map node read histograms from another nodes*/
void 
ReadHistogramFromNode( struct EachToOtherPattern *p_this, 
		       int nodetype, 
		       int index, 
		       int fdr ){
    WRITE_FMT_LOG( "eachtoother:read( from:index=%d, from:fdr=%d)\n", index, fdr );
    /*read histogram data from another nodes*/
    struct MapReduceData *mapred_data = (struct MapReduceData *)p_this->data;
    /*histogram has always the same index as node in nodes_list reading from*/
    assert( index < mapred_data->histograms_count );
    Histogram temp;
    Histogram *histogram = &mapred_data->histograms_list[index];
    WRITE_FMT_LOG("ReadHistogramFromNode index=%d\n", index);
    /*histogram array should be empty before reading*/
    assert( !histogram->buffer.header.count );
    read(fdr, &temp, sizeof(Histogram) );
    *histogram = temp;
    /*alloc buffer and receive data*/
    int res = AllocBuffer( &histogram->buffer, 
		 histogram->buffer.header.item_size, 
		 temp.buffer.header.count );
    IF_ALLOC_ERROR(res);
    read(fdr, histogram->buffer.data, histogram->buffer.header.buf_size );
    histogram->buffer.header.count = temp.buffer.header.count;
    WRITE_FMT_LOG("ReadHistogramFromNode count=%d\n", histogram->buffer.header.count);
}

/************************************************************************
 * EachToOtherPattern callbacks.
 * Every map node write histograms to another nodes*/
void 
WriteHistogramToNode( struct EachToOtherPattern *p_this, 
		      int nodetype, 
		      int index, 
		      int fdw ){
    WRITE_FMT_LOG( "eachtoother:write( to:index=%d, to:fdw=%d)\n", index, fdw );
    /*write own data histogram to another nodes*/
    struct MapReduceData *mapred_data = (struct MapReduceData *)p_this->data;
    /*nodeid in range from 1 up to count*/
    int own_histogram_index = p_this->conf->ownnodeid-1;
    assert( own_histogram_index < mapred_data->histograms_count );
    Histogram *histogram = &mapred_data->histograms_list[own_histogram_index];
    WRITE_FMT_LOG("WriteHistogramToNode index=%d\n", index);
    /*write Histogram struct, array contains pointer it should be ignored by receiver */
    write(fdw, histogram, sizeof(Histogram));
    /*write histogram data*/
    write(fdw, histogram->buffer.data, histogram->buffer.header.buf_size );
    WRITE_FMT_LOG( "eachtoother:wrote( count=%d)\n", histogram->buffer.header.count );
}


/*******************************************************************************
 * Summarize Histograms, shrink histogram and get as dividers array*/



/*@param divider_array space must preallocated by exact items count to be added;
 it's maximum value will be used by algorithm*/
void 
GetReducersDividerArrayBasedOnSummarizedHistograms( struct MapReduceUserIf *mif,
						    Histogram *histograms, 
						    int histograms_count, 
						    Buffer *divider_array ){
int CalculateItemsCountBasedOnHistograms(Histogram *histograms, int histograms_count );
int SearchMinimumHashAmongCurrentItemsOfAllHistograms( struct MapReduceUserIf *mif,
						       Histogram *histograms, 
						       int *current_indexes_array,
						       int histograms_count );

    WRITE_LOG("Create dividers list");
    assert(divider_array);
    int dividers_count_max = divider_array->header.buf_size / divider_array->header.item_size;
    assert(dividers_count_max != 0);
    size_t size_all_histograms_data = 0;
    size_all_histograms_data = 
	CalculateItemsCountBasedOnHistograms(histograms, histograms_count );
    /*calculate block size divider for single reducer*/
    size_t generic_divider_block_size = size_all_histograms_data / dividers_count_max;
    size_t current_divider_block_size = 0;

    /*start histograms processing*/
    int indexes[dividers_count_max];
    memset( &indexes, '\0', sizeof(indexes) );
    int histogram_index_with_minimal_hash = 0;
    
    /*calculate dividers in loop, last divider item must be added after loop 
      with maximum value as possible*/
    while( divider_array->header.count+1 < dividers_count_max ){
	histogram_index_with_minimal_hash = 
	    SearchMinimumHashAmongCurrentItemsOfAllHistograms( mif,
							       histograms, 
							       indexes, 
							       histograms_count );
	if ( histogram_index_with_minimal_hash == -1 ){
	    /*error - no more items in histograms, leave loop*/
	    break;
	}
	/*increase current divider block size*/
	if ( BOUNDS_OK(indexes[histogram_index_with_minimal_hash], 
		       histograms[histogram_index_with_minimal_hash].buffer.header.count-1)){
	    current_divider_block_size += histograms[histogram_index_with_minimal_hash]
		.step_hist_common;
	}
	else{
	    current_divider_block_size += histograms[histogram_index_with_minimal_hash]
		.step_hist_last;
	}

	if ( current_divider_block_size >= generic_divider_block_size ){
	    int min_item_index = indexes[histogram_index_with_minimal_hash];
	    Histogram* min_item_hist = &histograms[histogram_index_with_minimal_hash];
	    const uint8_t* divider_hash	
		= (const uint8_t*)BufferItemPointer(&min_item_hist->buffer, 
						    min_item_index);
	    /*add located divider value to dividers array*/
	    AddBufferItem( divider_array, divider_hash );
	    current_divider_block_size=0;

	    WRITE_FMT_LOG( "add divider item#%d from histogram#%d[%d], hashvalue=%s\n", 
			   divider_array->header.count-1,
			   histogram_index_with_minimal_hash, min_item_index,
			   PRINTABLE_HASH(mif, divider_hash) );
	    fflush(0);
	}
	indexes[histogram_index_with_minimal_hash]++;
	/*while pre last divider processing*/
    }

    /*Add last divider hash with maximum value, and it is guaranties that all rest 
      map values with hash value less than maximum will distributed into last reduce node 
      appropriated to last divider*/
    uint8_t* divider_hash = alloca( HASH_SIZE(mif) );
    memset( divider_hash, 0xffffff, HASH_SIZE(mif) );
    for(int i=divider_array->header.count; i < dividers_count_max; i++)
	AddBufferItem(divider_array, divider_hash);

#ifdef DEBUG
    WRITE_LOG("\ndivider_array=[");
    for( int i=0; i < divider_array->header.count; i++ ){
	const uint8_t* current_hash = (const uint8_t*)BufferItemPointer(divider_array, i);
	WRITE_FMT_LOG("%s ", PRINTABLE_HASH(mif,current_hash) );
    }
    WRITE_LOG("]\n");
#endif
}

int
CalculateItemsCountBasedOnHistograms(Histogram *histograms, int hist_count ){
    uint32_t ret=0;
    for ( int i=0; i < hist_count; i++ ){
	WRITE_FMT_LOG("histogram #%d items count=%u\n", i,
		      histograms[i].buffer.header.count );
	/*if histogram has more than one item*/
	if ( histograms[i].buffer.header.count > 1 ){
	    ret+= histograms[i].step_hist_common * (histograms[i].buffer.header.count -1); 
	    ret+= histograms[i].step_hist_last;			
	}								
	else if ( histograms[i].buffer.header.count == 1 ){		
	    assert( histograms[i].step_hist_last == histograms[i].step_hist_common ); 
	    ret += histograms[i].step_hist_last;			
	}							       
    }
    WRITE_FMT_LOG("all histograms items count=%u\n", ret);
    return ret;
}


int
SearchMinimumHashAmongCurrentItemsOfAllHistograms( struct MapReduceUserIf *mif,
						   Histogram *histograms, 
						   int *current_indexes_array,
						   int histograms_count){ 
    int res = -1;
    const uint8_t* current_hash;
    const uint8_t* minimal_hash = NULL;
    /*found minimal value among currently indexed histogram values*/
    for ( int i=0; i < histograms_count; i++ ){ /*loop for histograms*/
	/*check bounds of current histogram*/
	if ( BOUNDS_OK(current_indexes_array[i],
		       histograms[i].buffer.header.count) ){
	    /*get minimal value among currently indexed histogram values*/ 
	    current_hash = (const uint8_t*)BufferItemPointer( &histograms[i].buffer,
							      current_indexes_array[i]);
	    if ( !minimal_hash || 
		 HASH_CMP( mif, current_hash, minimal_hash ) <= 0 ){
		res = i;
		minimal_hash = current_hash;
	    }
	}
    }
    return res;
}


size_t
MapInputDataLocalProcessing( struct MapReduceUserIf *mif, 
			     const char *buf, 
			     size_t buf_size, 
			     int last_chunk, 
			     Buffer *result ){
    size_t unhandled_data_pos = 0;
    Buffer sort;
    int res = AllocBuffer( &sort, MRITEM_SIZE(mif), 1024 /*granularity*/ );
    IF_ALLOC_ERROR(res);

    WRITE_FMT_LOG("sbrk()=%p\n", sbrk(0) );
    WRITE_FMT_LOG("======= new portion of data read: input buffer=%p, buf_size=%u\n", 
		  buf, (uint32_t)buf_size );
    /*user Map process input data and allocate keys, values buffers*/
    unhandled_data_pos = mif->Map( buf, buf_size, last_chunk, &sort );
    WRITE_FMT_LOG("User Map() function result : items count=%u, unhandled pos=%u\n",
		  (uint32_t)sort.header.count, (uint32_t)unhandled_data_pos );

    WRITE_LOG_BUFFER(mif, sort );
    LocalSort( mif, &sort);

    WRITE_FMT_LOG("MapCallEvent:sorted map, count=%u\n", (uint32_t)sort.header.count);
    WRITE_LOG_BUFFER(mif, sort );

    res = AllocBuffer(result, MRITEM_SIZE(mif), sort.header.count/4 );
    IF_ALLOC_ERROR(res);

    WRITE_FMT_LOG("MapCallEvent: Combine Start, count=%u\n", (uint32_t)sort.header.count);
    if ( mif->Combine ){
	mif->Combine( &sort, result );
	FreeBufferData(&sort);
    }
    else{
	//use sort buffer instead reduced, because user does not defined Combine function
	*result = sort;
	sort.data = NULL;
    }

    WRITE_FMT_LOG("MapCallEvent: Combine Complete, count=%u\n", (uint32_t)result->header.count);
    WRITE_LOG_BUFFER(mif, *result );
    return unhandled_data_pos;
}


void 
MapCreateHistogramSendEachToOtherCreateDividersList( struct ChannelsConfigInterface *ch_if, 
						     struct MapReduceUserIf *mif, 
						     const Buffer *map ){
    /*retrieve Map/Reducer nodes list and nodes count*/
    int *map_nodes_list = NULL;
    int map_nodes_count = ch_if->GetNodesListByType( ch_if, EMapNode, &map_nodes_list );
    assert(map_nodes_count); /*it should not be zero*/

    /*get reducers count, this same as dividers count for us, reducers list is not needed now*/
    int *reduce_nodes_list = NULL;
    int reduce_nodes_count = 
	ch_if->GetNodesListByType( ch_if, EReduceNode, &reduce_nodes_list );

    //generate histogram with offset
    int current_node_index = ch_if->ownnodeid-1;

    struct Histogram* histogram = &mif->data.histograms_list[current_node_index];

    size_t hist_step = map->header.count / 100 / map_nodes_count;
    /*save histogram for own data always into current_node_index-pos of histograms array*/
    GetHistogram( mif, map, hist_step, histogram );

    WRITE_FMT_LOG("MapCallEvent: histogram created. "
		  "count=%d, fstep=%u, lstep=%u\n",
		  (int)histogram->buffer.header.count,
		  (uint16_t)histogram->step_hist_common,
		  (uint16_t)histogram->step_hist_last );

    /*use eachtoother pattern to map nodes communication*/
    struct EachToOtherPattern histogram_from_all_to_all_pattern = {
	.conf = ch_if,
	.data = &mif->data,
	.Read = ReadHistogramFromNode,
	.Write = WriteHistogramToNode
    };
    StartEachToOtherCommunication( &histogram_from_all_to_all_pattern, EMapNode );

    /*Preallocate buffer space and exactly for items count=reduce_nodes_count */
    int res = AllocBuffer( &mif->data.dividers_list, HASH_SIZE(mif), reduce_nodes_count);
    IF_ALLOC_ERROR(res);
    /*From now every map node contain histograms from all map nodes, summarize histograms,
     *to get distribution of all data. Result of summarization write into divider_array.*/
    GetReducersDividerArrayBasedOnSummarizedHistograms( mif,
							mif->data.histograms_list,
							mif->data.histograms_count,
							&mif->data.dividers_list );
    free(map_nodes_list);
    free(reduce_nodes_list);
}

static int
BufferedWriteSingleMrItem( BufferedIOWrite* bio, 
			   int fdw,
			   const ElasticBufItemData* item,
			   int hashsize,
			   int value_addr_is_data){
    int bytes=0;
    /*key size*/
    bytes+=bio->write( bio, fdw, (void*)&item->key_data.size, sizeof(item->key_data.size));
    /*key data*/
    bytes+=bio->write( bio, fdw, (void*)item->key_data.addr, item->key_data.size);

    if ( value_addr_is_data ){
	/*ElasticBufItemData::key_data::addr used as data, key_data::size not used*/
	bytes+=bio->write( bio, fdw, (void*)&item->value.addr, sizeof(item->value.addr) );
    }
    else{
	/*value size*/
	bytes+=bio->write( bio, fdw, (void*)&item->value.size, sizeof(item->value.size));
	/*value data*/
	bytes+=bio->write( bio, fdw,(void*)item->value.addr, item->value.size);
    }
    /*hash of key*/
    bytes+=bio->write( bio, fdw, (void*)&item->key_hash, hashsize );
    return bytes;
}

static int
BufferedReadSingleMrItem( BufferedIORead* bio, 
			  int fdr,
			  ElasticBufItemData* item,
			  int hashsize,
			  int value_addr_is_data){
    int bytes=0;
    /*key size*/
    bytes += bio->read( bio, fdr, (void*)&item->key_data.size, sizeof(item->key_data.size));
    /*key data*/
    item->key_data.addr = (uintptr_t)malloc(item->key_data.size);
    item->own_key = EDataOwned;
    bytes += bio->read( bio, fdr, (void*)item->key_data.addr, item->key_data.size);

    if ( value_addr_is_data ){
	/*ElasticBufItemData::key_data::addr used as data, key_data::size not used*/
	bytes += bio->read( bio, fdr, (void*)&item->value.addr, sizeof(item->value.addr) );
	item->own_value = EDataNotOwned;
	item->value.size = 0;
    }
    else{

	/*value size*/
	bytes += bio->read( bio, fdr, (void*)&item->value.size, sizeof(item->value.size));
	/*value data*/
	item->value.addr = (uintptr_t)malloc(item->value.size);
	item->own_value=EDataOwned;
	bytes += bio->read( bio, fdr, (void*)item->value.addr, item->value.size);
    }
    /*key hash*/
    bytes += bio->read( bio, fdr, (void*)&item->key_hash, hashsize );
    return bytes;
}
 

static size_t
CalculateSendingDataSize(const Buffer *map, 
			 int data_start_index, 
			 int items_count,
			 int hashsize,
			 int value_addr_is_data)
{
    /*calculate sending data of size */
    size_t senddatasize= 0;
    senddatasize += sizeof(int) + sizeof(int); //last_data_flag,items count
    int loop_up_to_count = data_start_index+items_count;
    const ElasticBufItemData* item;

    for( int i=data_start_index; i < loop_up_to_count; i++ ){
	item = (const ElasticBufItemData*)BufferItemPointer( map, i );
	senddatasize+= item->key_data.size;     /*size of key data*/
	if ( !value_addr_is_data )
	    senddatasize+= item->value.size;    /*size of value data*/
    }
    /*size of value data / value size*/
    //value data/size
    if ( value_addr_is_data )
	senddatasize+= items_count * sizeof(((struct BinaryData*)0)->addr);
    else
	senddatasize+= items_count * sizeof(((struct BinaryData*)0)->size);
    /*size of hash*/
    senddatasize+= items_count * hashsize;
    /*size of key size*/
    senddatasize+= items_count * sizeof(((struct BinaryData*)0)->size);
    /*************************************/
    WRITE_FMT_LOG( "senddatasize=%d\n", senddatasize );
    return senddatasize;
}
 
void 
WriteDataToReduce( struct MapReduceUserIf *mif,
		   BufferedIOWrite* bio, 
		   int fdw, 
		   const Buffer *map, 
		   int data_start_index, 
		   int items_count,
		   int last_data_flag ){
    int bytes=0;
    ElasticBufItemData* current = alloca( MRITEM_SIZE(mif) );
    int loop_up_to_count = data_start_index+items_count;
    assert( loop_up_to_count <= map->header.count );

    int senddatasize = CalculateSendingDataSize(map, 
						data_start_index, 
						items_count, 
						HASH_SIZE(mif),
						mif->data.value_addr_is_data);

    /*log first and last hashes of range to send */
#ifdef DEBUG
    WRITE_FMT_LOG( "data_start_index=%d, items_count=%d\n", 
		   data_start_index, items_count );
    ElasticBufItemData* temp = alloca( MRITEM_SIZE(mif) );
    GetBufferItem( map, data_start_index, current );
    GetBufferItem( map, data_start_index+items_count-1, temp ); /*last item from range*/
    WRITE_FMT_LOG( "send range of hashes [%s - %s]",
		   PRINTABLE_HASH(mif, &current->key_hash), 
		   PRINTABLE_HASH(mif, &temp->key_hash) );
    WRITE_FMT_LOG( "fdw=%d, last_data_flag=%d, items_count=%d\n", 
		   fdw, last_data_flag, items_count );
#endif //DEBUG

    bio->write( bio, fdw, &senddatasize, sizeof(int) );
    
    /*write last data flag 0 | 1, if reducer receives 1 then it
      should exclude this map node from communications*/
    bytes+= bio->write( bio, fdw, &last_data_flag, sizeof(int) );
    /*items count we want send to a single reducer*/
    bytes+= bio->write( bio, fdw, &items_count, sizeof(int) );

    for( int i=data_start_index; i < loop_up_to_count; i++ ){
	GetBufferItem( map, i, current );
	bytes+= BufferedWriteSingleMrItem( bio, fdw, current, 
					   HASH_SIZE(mif),
					   mif->data.value_addr_is_data);
    }
    bio->flush_write(bio, fdw);
    assert(bytes ==senddatasize);
}

/*struct to be used inside MapSendToAllReducers*/
struct BasketInfo{
    int data_start_index;
    int count_in_section;
};

static void 
MapSendToAllReducers( struct ChannelsConfigInterface *ch_if, 
		      struct MapReduceUserIf *mif,
		      int last_data, 
		      const Buffer *map){
    /*Distribute data to Reducer nodes, using dividers data*/
    struct BasketInfo* DistributeDataIntoBaskets(struct MapReduceUserIf *mif, 
						 const Buffer *map,
						 int basket_count);

    int basket_count = mif->data.dividers_list.header.count;
    struct BasketInfo* basket_array = DistributeDataIntoBaskets( mif, map, basket_count);
    last_data = last_data ? MAP_NODE_EXCLUDE : MAP_NODE_NO_EXCLUDE; 
  
    /*get reducers count, this is same as dividers count for us*/
    int *reduce_nodes_list = NULL;
    int dividers_count = ch_if->GetNodesListByType( ch_if, EReduceNode, &reduce_nodes_list );
    assert(reduce_nodes_list);
    WRITE_FMT_LOG("dividers_count=%d, basket_count=%d\n", dividers_count, basket_count);
    assert(dividers_count==basket_count);

    /*Declare big buffer to provide efficient buffered IO, free it at the function end*/
    void *send_buffer; 
    send_buffer= malloc(SEND_BUFFER_SIZE);
    IF_ALLOC_ERROR(send_buffer?0:SEND_BUFFER_SIZE);
    BufferedIOWrite* bio = AllocBufferedIOWrite( send_buffer, SEND_BUFFER_SIZE);
    IF_ALLOC_ERROR( bio?0:SEND_BUFFER_SIZE );

    for( int i=0; i < basket_count; i++ ){
	/*send to reducer with current_divider_index index*/
	struct UserChannel *channel 
	    = ch_if->Channel(ch_if, EReduceNode, reduce_nodes_list[i], EChannelModeWrite);
	assert(channel);
	int fdw = channel->fd;
	WRITE_FMT_LOG( "write to fdw=%d /reducer node=%d, %d items\n", fdw,
		       reduce_nodes_list[i], basket_array[i].count_in_section );
	WriteDataToReduce( mif,
			   bio,
			   fdw, 
			   map, 
			   basket_array[i].data_start_index, 
			   basket_array[i].count_in_section, 
			   last_data );
    }

    free(bio);
    free(send_buffer);
    free(reduce_nodes_list);
}


/*Depend on mif->data.dividers_list calculate inex of beginning item index and items
 *count of buffer map to be send into reducer node in further, 
 *@param resulted array with calculation info*/
struct BasketInfo* DistributeDataIntoBaskets(struct MapReduceUserIf *mif, 
					     const Buffer *map,
					     int basket_count){

#define LOG_ADD_TO_NEW_BASKET(mif_p, map_p, itemindex, basketindex, dataindex, countsection, \
			      currentdivider)				\
	{								\
	    if ( itemindex > 0 ){					\
		ElasticBufItemData* previtem				\
		    = (ElasticBufItemData*)BufferItemPointer( map_p, itemindex-1); \
		WRITE_FMT_LOG( "reducer #%d divider_hash=%s, [%d]=%s\n", \
			       basketindex,				\
			       PRINTABLE_HASH(mif_p, currentdivider),	\
			       itemindex-1, PRINTABLE_HASH(mif, &previtem->key_hash) );	\
	    }								\
	    WRITE_FMT_LOG( "reducer #%d divider_hash=%s, [%d]=%s\n",	\
			   basketindex,					\
			   PRINTABLE_HASH(mif_p, currentdivider),	\
			   itemindex, PRINTABLE_HASH(mif_p, &current->key_hash) ); \
	    WRITE_FMT_LOG( "Basket #%d start_index=%d, count=%d\n",	\
			   basketindex, dataindex, countsection );	\
	}								\

    int switchbasket=0;
    int basket_index=0;
    int data_start_index = 0;
    int count_in_section = 0;
    int current_divider_index = 0;
    ElasticBufItemData* current = alloca( MRITEM_SIZE(mif) );
    void*  current_divider_hash = alloca( HASH_SIZE(mif) );
    struct BasketInfo* basket_array;
    basket_array = malloc(sizeof(struct BasketInfo)*basket_count);

    /*Get first divider hash*/
    GetBufferItem(&mif->data.dividers_list, 0, current_divider_hash);

    /*loop for data +1, no out of bounds can be because handle it*/
    for ( int j=0; j < map->header.count+1; j++ ){
	if ( j < map->header.count ){
	    /*get item by valid index*/
	    GetBufferItem( map, j, current);
	    if ( HASH_CMP(mif, &current->key_hash, current_divider_hash ) <= 0 ){
		switchbasket=0;
		count_in_section++;
	    }
	    /*switch to new basket, cuurent hash more than divider*/
	    else{
		switchbasket=1;
	    }
	}
	else{
	    switchbasket=1;
	}

	/*if current item is last OR current hash more than current divider key*/
	if ( switchbasket != 0 ){
#ifdef DEBUG
	    LOG_ADD_TO_NEW_BASKET(mif, map, j, basket_index, data_start_index, 
				  count_in_section, current_divider_hash);
#endif
	    struct BasketInfo info;
	    info.data_start_index = data_start_index;
	    info.count_in_section = count_in_section;
	    basket_array[basket_index++] = info;

	    /*switch to next divider, do increment if it's not last handling item*/
	    current_divider_index++;
	    if ( current_divider_index < mif->data.dividers_list.header.count ){
		/*retrieve hash value by updated index*/
		GetBufferItem( &mif->data.dividers_list, 
			       current_divider_index, 
			       current_divider_hash);
	    }
	    /*update data start index for new divider*/
	    data_start_index = j;
	    /*reset count for next divider, also count current item */
	    count_in_section = 1;
	}
    }

    /*add info about empty data to the rest of baskets*/
    for(int i=basket_index; i < basket_count; i++ ){
	struct BasketInfo info;
	info.data_start_index = 0;
	info.count_in_section = 0;
	basket_array[basket_index++] = info;
    }

    return basket_array;
}



void 
InitMapInternals( struct MapReduceUserIf *mif, 
		  const struct ChannelsConfigInterface *chif, 
		  struct MapNodeEvents* ev){
    int res;
    /*get histograms count for MapReduceData*/
    int *nodes_list_unwanted = NULL;
    mif->data.histograms_count = 
	chif->GetNodesListByType(chif, EMapNode, &nodes_list_unwanted );
    assert(mif->data.histograms_count>0);
    free(nodes_list_unwanted);
    /*init histograms list*/
    mif->data.histograms_list = calloc( mif->data.histograms_count, 
					 sizeof(*mif->data.histograms_list) );
    for(int i=0; i<mif->data.histograms_count; i++){
	res = AllocBuffer(&mif->data.histograms_list[i].buffer, HASH_SIZE(mif), 100);
	IF_ALLOC_ERROR(res);
    }

    ev->MapCreateHistogramSendEachToOtherCreateDividersList 
	= MapCreateHistogramSendEachToOtherCreateDividersList;
    ev->MapInputDataLocalProcessing = MapInputDataLocalProcessing;
    ev->MapInputDataProvider = MapInputDataProvider;
    ev->MapSendToAllReducers = MapSendToAllReducers;
}


void
PrintDebugInfo(struct MapReduceUserIf *mif, struct ChannelsConfigInterface *chif){
    WRITE_FMT_LOG("MrItemSize =%dbytes\n", mif->data.mr_item_size);
    WRITE_FMT_LOG("HashSize =%dbytes\n", mif->data.hash_size);

    /*get map_nodes_count*/
    int *map_nodes_list = NULL;
    int map_nodes_count = chif->GetNodesListByType( chif, EMapNode, &map_nodes_list);
    free(map_nodes_list);
    WRITE_FMT_LOG("MapNodesCount= %d\n", map_nodes_count);

    /*get reduce_nodes_count*/
    int *reduce_nodes_list = NULL;
    int reduce_nodes_count = chif->GetNodesListByType( chif, EReduceNode, &reduce_nodes_list);
    free(reduce_nodes_list);
    WRITE_FMT_LOG("ReduceNodesCount= %d\n", reduce_nodes_count);
}


int 
MapNodeMain( struct MapReduceUserIf *mif, 
	     struct ChannelsConfigInterface *chif ){
    WRITE_LOG("MapNodeMain\n");
    assert(mif);
    assert(mif->Map);
    /*Combine function can be not defined*/
    assert(mif->Reduce);
    assert(mif->ComparatorHash);
    assert(mif->ComparatorMrItem);
#ifndef DISABLE_WRITE_LOG_BUFFER
    assert(mif->DebugHashAsString);
#endif

    struct MapNodeEvents events;
    InitMapInternals(mif, chif, &events);

    PrintDebugInfo(mif, chif);

    /*should be initialized at fist call of DataProvider*/
    char *buffer = NULL;
    size_t returned_buf_size = 0;
    /*default block size for input file*/
    size_t split_input_size=DEFAULT_MAP_CHUNK_SIZE_BYTES;

    /*get from environment block size for input file*/
    if ( getenv(MAP_CHUNK_SIZE_ENV) )
	split_input_size = atoi(getenv(MAP_CHUNK_SIZE_ENV));
    WRITE_FMT_LOG( "MAP_CHUNK_SIZE_BYTES=%d\n", split_input_size );
	
    /*by default can set any number, but actually it should be point to start of unhandled data,
     * for fully handled data it should be set to data size*/
    size_t current_unhandled_data_pos = 0;
    int last_chunk = 0;

    /*get input channel*/
    struct UserChannel *channel = chif->Channel(chif,
						EInputOutputNode, 
						1, 
						EChannelModeRead);
    assert(channel);

    /*read input data*/
    do{
	free(buffer), buffer=NULL;

	/*last parameter is not used for first call, 
	  for another calls it should be assigned by user returned value of Map call*/
	returned_buf_size = events.MapInputDataProvider(
							channel->fd,
							&buffer,
							split_input_size,
							current_unhandled_data_pos
							);
	last_chunk = returned_buf_size < split_input_size? 1 : 0; //last chunk flag
	if ( last_chunk != 0 ){
	    WRITE_LOG( "MapInputDataProvider last chunk data" );
	}

	/*prepare Buffer before using by user Map function*/
	Buffer map_buffer;

	if ( returned_buf_size ){
	    /*call users Map, Combine functions only for non empty data set*/
	    current_unhandled_data_pos = 
		events.MapInputDataLocalProcessing( mif,
						    buffer, 
						    returned_buf_size,
						    last_chunk,
						    &map_buffer );
	}

	if ( !mif->data.dividers_list.header.count ){
	    events.MapCreateHistogramSendEachToOtherCreateDividersList( chif, 
									mif, 
									&map_buffer );
	}

	/*based on dividers list which helps easy distribute data to reduce nodes*/
	events.MapSendToAllReducers( chif, 
				     mif,
				     last_chunk, 
				     &map_buffer);

	for(int i=0; i < map_buffer.header.count; i++){
	    TRY_FREE_MRITEM_DATA( (ElasticBufItemData*) 
				  BufferItemPointer(&map_buffer, i ) );
	}
	FreeBufferData(&map_buffer);
    }while( last_chunk == 0 );

    WRITE_LOG("MapNodeMain Complete\n");

    return 0;
}

/*Copy into dest buffer items from source_arrays buffers in sorted order*/
void 
MergeBuffersToNew( struct MapReduceUserIf *mif,
		   Buffer *dest,
		   const Buffer *source_arrays, 
		   int arrays_count ){
    int SearchMinimumHashAmongCurrentItemsOfAllMergeBuffers( struct MapReduceUserIf *mif,
							     const Buffer* merge_buffers, 
							     int *current_indexes_array,
							     int count);
    int all_items_count=0;
    /*List of items count of every merging item before merge*/
    int i;
    for(i=0; i < arrays_count; i++ ){
	all_items_count += source_arrays[i].header.count;
	WRITE_FMT_LOG( "Before merge: source[#%d], keys count %d\n", 
		       i, source_arrays[i].header.count );
    }    
    /*alloc buffer for all items*/
    int ret = AllocBuffer( dest, MRITEM_SIZE(mif), all_items_count );
    IF_ALLOC_ERROR(ret);

    int merge_pos[arrays_count];
    memset(merge_pos, '\0', sizeof(merge_pos) );
    int min_key_array;
    ElasticBufItemData* current;

    /*search minimal key among current items of source_arrays*/
    min_key_array =
	SearchMinimumHashAmongCurrentItemsOfAllMergeBuffers( mif,
							     source_arrays, 
							     merge_pos,
							     arrays_count);
    uint32_t merge_result_bytes_occupied=0;
    while( min_key_array !=-1 ){
	/*copy item data with minimal key into destination buffer*/
	current = (ElasticBufItemData*)
	    BufferItemPointer( &source_arrays[min_key_array], merge_pos[min_key_array] );
	AddBufferItem( dest, current );
	merge_pos[min_key_array]++;
	min_key_array =
	    SearchMinimumHashAmongCurrentItemsOfAllMergeBuffers( mif,
								 source_arrays, 
								 merge_pos,
								 arrays_count);
	merge_result_bytes_occupied += mif->data.mr_item_size + current->key_data.size;
	if( !mif->data.value_addr_is_data )
	    merge_result_bytes_occupied += current->value.size;
    }
    WRITE_FMT_LOG("Merged Items memory occupied=%u\n", merge_result_bytes_occupied);
}

int
SearchMinimumHashAmongCurrentItemsOfAllMergeBuffers( struct MapReduceUserIf *mif,
						     const Buffer* merge_buffers, 
						     int *current_indexes_array,
						     int count){ 
    int res = -1;
    ElasticBufItemData* item;
    const uint8_t* current_hash;
    const uint8_t* minimal_hash = NULL;
    for ( int i=0; i < count; i++ ){ /*loop for merge arrays*/
	/*check bounds of current buffer*/
	if ( BOUNDS_OK(current_indexes_array[i],merge_buffers[i].header.count) ){
	    /*get minimal value among currently indexed histogram values*/ 
		item = (ElasticBufItemData*)BufferItemPointer( &merge_buffers[i],
							       current_indexes_array[i]);
		current_hash =  (const uint8_t*)&item->key_hash;
	    if ( minimal_hash == NULL || HASH_CMP( mif, current_hash, minimal_hash ) <= 0 ){
		res = i;
		minimal_hash = current_hash;
	    }
	}
    }
    return res;
}


static exclude_flag_t
RecvDataFromSingleMap( struct MapReduceUserIf *mif,
		       int fdr,
		       Buffer *map) {
    exclude_flag_t excl_flag;
    int items_count;
    char* recv_buffer;
    int bytes;
    /*read exact packet size and allocate buffer to read a whole data by single read i/o*/
    read(fdr, &bytes, sizeof(int) );
    WRITE_FMT_LOG( "packet size=%d\n", bytes );
    if ( bytes > 0 ){
	recv_buffer = malloc(bytes);
	IF_ALLOC_ERROR(recv_buffer?0:bytes);

	BufferedIORead* bio = AllocBufferedIORead( recv_buffer, bytes);
	IF_ALLOC_ERROR(bio?0:bytes);
	/*read last data flag 0 | 1, if reducer receives 1 then it should
	 * exclude sender map node from communications in further*/
	bytes=0;
	bytes += bio->read( bio, fdr, &excl_flag, sizeof(int) );
	/*read items count*/
	bytes += bio->read( bio, fdr, &items_count, sizeof(int));
	WRITE_FMT_LOG( "readmap exclude flag=%d, items_count=%d\n", excl_flag, items_count );

	/*alloc memory for all array cells expected to receive, if no items will recevied
	 *initialize buffer anyway*/
	int res = AllocBuffer(map, MRITEM_SIZE(mif), items_count);
	IF_ALLOC_ERROR(res);

	if ( items_count > 0 ){
	    /*read items_count items*/
	    ElasticBufItemData* item;
	    for( int i=0; i < items_count; i++ ){
		/*add item manually, no excesive copy doing*/
		AddBufferItemVirtually(map);
		item = (ElasticBufItemData*)BufferItemPointer( map, i );
		/*save directly into array*/
		bytes+=BufferedReadSingleMrItem( bio, fdr, item, 
						 HASH_SIZE(mif),
						 mif->data.value_addr_is_data);
	    }
	}
	WRITE_FMT_LOG( "readed %d bytes from Map node, fdr=%d, item count=%d\n", 
		       bytes, fdr, map->header.count );
	WRITE_LOG_BUFFER( mif, *map );
	free(bio);
	free(recv_buffer);
    }
    return excl_flag;
}

/*into newbuf_p will be added all from merge_buffers_p and from all buffer in sort order*/
#define MERGE_PREVIOUS_AND_NEW(mif_p, merge_buffers_p, count_buffers, prevbuf, newbuf ){ \
	/*grow array of buffers, and add previous merge result into merge array*/ \
	merge_buffers_p = realloc( merge_buffers_p,			\
				   (++count_buffers)*sizeof(Buffer) );	\
	merge_buffers_p[count_buffers-1] = prevbuf;			\
	memset( &prevbuf, '\0', sizeof(prevbuf) );			\
	WRITE_LOG( "Data received from mappers, merge it" );		\
	/*Alloc buffers for merge results, and merge previous and new data*/ \
	MergeBuffersToNew(mif_p, &newbuf, merge_buffers_p, count_buffers ); \
	WRITE_FMT_LOG( "merge complete, keys count %d, data=%p\n", newbuf.header.count, newbuf.data ); \
	/*Merge complete, free source recv buffers*/			\
	for ( int i=0; i < count_buffers; i++ ){			\
	    FreeBufferData(&merge_buffers_p[i]);			\
	}								\
	/*reset merge buffers*/						\
	free(merge_buffers_p), merge_buffers_p = NULL;			\
	count_buffers=0;						\
    }


int 
ReduceNodeMain( struct MapReduceUserIf *mif, 
		struct ChannelsConfigInterface *chif ){
    WRITE_LOG("ReduceNodeMain\n");
    assert(mif);
    assert(mif->Map);
    /*Combine function can be not defined*/
    assert(mif->Reduce);
    assert(mif->ComparatorHash);
    assert(mif->ComparatorMrItem);
#ifndef DISABLE_WRITE_LOG_BUFFER
    assert(mif->DebugHashAsString);
#endif

#ifdef DEBUG
    PrintDebugInfo(mif, chif);
#endif

    /*get map_nodes_count*/
    int *map_nodes_list = NULL;
    int map_nodes_count = chif->GetNodesListByType( chif, EMapNode, &map_nodes_list);

    /*buffers_for_merge is an arrays received from Mappers to be merged 
     *it will grow runtime */
    Buffer *merge_buffers = NULL;
    int merge_buffers_count=0;
    /*Buffer for merge*/
    Buffer merged;
    /*Buffer for sorted*/
    Buffer all; memset( &all, '\0', sizeof(all) );

    int excluded_map_nodes[map_nodes_count];
    memset( excluded_map_nodes, '\0', sizeof(excluded_map_nodes) );

    /*read data from map nodes*/
    int leave_map_nodes; /*is used as condition for do while loop*/
    do{
	leave_map_nodes = 0;
	for( int i=0; i < map_nodes_count; i++ ){
	    /*If expecting data from current map node*/
	    if ( excluded_map_nodes[i] != MAP_NODE_EXCLUDE ){
		struct UserChannel *channel 
		    = chif->Channel(chif, EMapNode, map_nodes_list[i], EChannelModeRead );
		assert(channel);
		WRITE_FMT_LOG( "Read [%d]map#%d, fdr=%d\n", 
			       i, map_nodes_list[i], channel->fd );

		/*grow array of buffers, and always receive items into new buffer*/
		merge_buffers = realloc( merge_buffers, 
					 (++merge_buffers_count)*sizeof(Buffer) );
		excluded_map_nodes[i] 
		    = RecvDataFromSingleMap( mif, 
					     channel->fd, 
					     &merge_buffers[merge_buffers_count-1] );
		
		/*set next wait loop condition*/
		if ( excluded_map_nodes[i] != MAP_NODE_EXCLUDE ){
		    /* have mappers expected to send again*/
		    leave_map_nodes = 1;
		}
	    }
	}//for

	/**********************************************************/
	/*combine data every time while it receives from map nodes*/
	if ( mif->Combine ){
	    MERGE_PREVIOUS_AND_NEW(mif, merge_buffers, merge_buffers_count, all, merged );

	    int granularity = all.header.count>0? all.header.count/3 : 1000;
	    int ret = AllocBuffer( &all, MRITEM_SIZE(mif), granularity );
	    IF_ALLOC_ERROR(ret);

	    WRITE_LOG_BUFFER(mif,merged);
	    WRITE_FMT_LOG( "keys count before Combine: %d\n", 
			   (int)merged.header.count );
	    mif->Combine( &merged, &all );
	    FreeBufferData( &merged );
	    WRITE_FMT_LOG( "keys count after Combine: %d\n", (int)all.header.count );
	    WRITE_LOG_BUFFER(mif,all);
	}else{
	    WRITE_LOG( "Combine function not defined and skipped" );
	}
	WRITE_FMT_LOG("sbrk()=%p\n", sbrk(0) );
    }while( leave_map_nodes != 0 );

    /*do merge only once if Combine not defined*/
    if ( !mif->Combine ){
	MERGE_PREVIOUS_AND_NEW(mif, merge_buffers, merge_buffers_count, all, merged );
	/*assign data from 'merged' buffer into 'all' buffer*/
	FreeBufferData(&all);
	all = merged;
    }

    if( mif->Reduce ){
	/*user should output data into output file/s*/
	WRITE_FMT_LOG( "Reduce : %d items, data=%p\n", (int)all.header.count, all.data );
	mif->Reduce( &all );
    }

    free(map_nodes_list);
    FreeBufferData(&all);

    WRITE_LOG("ReduceNodeMain Complete\n");
    return 0;
}

