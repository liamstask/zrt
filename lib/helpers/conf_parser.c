/*
 * conf_parser.c
 *
 *  Created on: 5.12.2012
 *      Author: YaroslavLitvinov
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "zrtlog.h"
#include "zrt_helper_macros.h"
#include "conf_parser.h"

enum ParsingStatus{
    EStComment,
    EStProcessing,
    EStToken
};

/*****************************************
 * keys of params related to single record
 *****************************************/

void add_key_to_list(struct KeyList* list, const char* key){
    assert(list);
    assert(list->key_count < MAX_PARAMS_COUNT);
    /*alloc memory and copy key to list*/
    list->key_list[list->key_count] = calloc( strlen(key)+1, 1 );
    strcpy( list->key_list[list->key_count], key );
    /*increment added keys count*/
    ++list->key_count;
}

void free_keylist(struct KeyList* list){
    assert(list);
    int i;
    for( i=0; i < list->key_count; i++ ){
	free(list->key_list[i]);
    }
}

struct internal_parse_data{
    char* key;
    uint16_t keylen;
    char* val;
    uint16_t vallen;
};

int key_index_from_key_name(const struct KeyList* keys, const char* key, int keylen){
    assert(keys);
    int i;
    for(i=0; i < keys->key_count; i++ ){
        /*compare key ignoring case*/
	int etalonkeylen = strlen(keys->key_list[i]);
        if ( !strncasecmp(key, keys->key_list[i], etalonkeylen) &&
	     keylen == etalonkeylen ){
	    return i; /*get key index, specified key macthed*/
        }
    }
    return -1; /*wrong key*/
}


/********************************************
 * parsing helpers
 ********************************************/
const char* strip_all(const char* str, int len, uint16_t* striped_len ){
    int begin, end;
    /*strip from begin*/
    for( begin=0; begin < len; begin++ ){
        if ( str[begin] != ' ' && str[begin] != '\t' && str[begin] != '\n' ) break;
    }
    /*strip from end*/
    for( end=len-1; len > 0; end-- ){
        if ( str[end] != ' ' && str[end] != '\t' && str[end] != '\n' ) break;
    }
    *striped_len = end-begin+1;
    return MAX(0, str+begin);
}

/*return 0 if extracted, -1 if not*/
int extract_key_value( const char* src, int src_len,
        char** key, uint16_t* key_len, char** val, uint16_t*val_len ){
    /* just search '=' char, all before that will key, and rest will value*/
    char* delim = strchr(src, '=');
    if ( delim == NULL ) return -1; /*not valid pair*/
    int keylen = delim-src;
    /*keylen is full length, key_len, val_en are striped lengths*/
    *key = (char*)strip_all(src, keylen, key_len );
    *val = (char*)strip_all(delim+1, src_len-keylen-1, val_len );
    return 0;
}

/*****************************************
 * parse and save parsed data
 *****************************************/

struct ParsedRecord* 
save_parsed_param_into_record(const struct KeyList* keys,
			      struct internal_parse_data* params_array, 
			      int params_count){
    assert(keys);
    struct ParsedRecord* record = NULL;
    int i;
    for(i=0; i < params_count; i++ ){
	int key_index =  key_index_from_key_name(keys, 
						 params_array[i].key, 
						 params_array[i].keylen);
	/*if current key is wrong*/
	if ( key_index < 0 ){
	    char* wrong_key = malloc(params_array[i].keylen+1);
	    strcpy(wrong_key, params_array[i].key);
	    ZRT_LOG(L_ERROR, "conf_parser: not valid key '%s'", wrong_key);
	    free(wrong_key);
	    /*free record memories*/
	    if (record){
		free(record->parsed_params_array);
		free(record);
	    }
	    return NULL; /*error*/
	}
	else{
	    /*alloc record if not yet alloced*/
	    if ( record == NULL ){
		/*alloc record*/
		record = malloc(sizeof(struct ParsedRecord));
		/*alloc params array*/
		record->parsed_params_array = malloc(sizeof(struct ParsedParam)*params_count);
	    }
	    /*save current param*/
	    record->parsed_params_array[key_index].key_index = key_index; /*param index*/
	    record->parsed_params_array[key_index].val = params_array[i].val;
	    record->parsed_params_array[key_index].vallen = params_array[i].vallen;
	}
    }
    return record;
}


struct ParsedRecord* conf_parse(const char* text, int len, struct KeyList* key_list,
				int* parsed_records_count){
    assert(text);
    assert(key_list);
    int error = 0;
    int lex_cursor = 0;
    int lex_length = 0;
    struct ParsedRecord* records = NULL;
    /*alloc array of params for single record*/
    struct internal_parse_data* temp_parsed = 
	malloc(sizeof(struct internal_parse_data)*key_list->key_count); 
    int parsed_params_count=0;
    enum ParsingStatus st = EStProcessing;
    enum ParsingStatus st_prev = st;

    ZRT_LOG(L_INFO, P_TEXT, "parsing");
    int cursor=0;
    do {
        /*set comment stat if comment start found*/
        if ( text[cursor] == '#' ){
	    if ( st == EStProcessing ){
		st_prev = EStComment;
		st = EStToken;
	    }
	    else{
		st = EStComment;
	    }
	    ZRT_LOG(L_EXTRA, "cursor=%d EStComment", cursor);
        }
        /* If comma in non comment state OR 
	 * for new line OR 
	 * if previos state was complete token 
         * then set processing state to catch new processing data by cursor position*/
        else if ( (st != EStComment && text[cursor] == ',') ||
		  text[cursor] == '\n' ){
	    if ( st == EStProcessing ){
		/*if now processing data then handle it*/
		ZRT_LOG(L_EXTRA, "cursor=%d EStToken", cursor);
		st_prev = EStProcessing;
		st = EStToken;
	    }
	    else{
		/*start processing of significant data*/
		ZRT_LOG(L_EXTRA, "cursor=%d EStProcessing", cursor);
		st = EStProcessing;
		/*save begin of unprocessed data to know start position of unhandled data*/
		lex_cursor = cursor+1;
		/*set lex_length -1. it's will be incremented in 
		 *switch EStProcessing and lex_length value become valid*/
		lex_length=-1; 
	    }
        }
	/*parsing data right bound reached*/
	else if (cursor == len-1){
	    /*extend current token up to last char*/
	    ++lex_length;
	    st = EStToken;
	}
        ++cursor;

        switch(st){
        case EStProcessing:
            ++lex_length;
            break;
        case EStComment:
            lex_cursor = -1;
            lex_length=0;
            break;
        case EStToken:{
	    ZRT_LOG(L_INFO, "swicth EStToken: lex_cursor=%d, lex_length=%d, pointer=%p", 
		    lex_cursor, lex_length, &text[lex_cursor]);
	    /*log non-striped lexema*/
	    char* non_striped_lexem_text = calloc(lex_length+1, 1);
	    memcpy(non_striped_lexem_text, &text[lex_cursor], lex_length);
	    ZRT_LOG(L_SHORT, "lex= '%s'", non_striped_lexem_text);
	    free(non_striped_lexem_text);

	    uint16_t striped_token_len=0;
	    const char* striped_token = 
		strip_all(&text[lex_cursor], lex_length, &striped_token_len );
	    ZRT_LOG(L_INFO, "swicth EStToken: striped token len=%d, pointer=%p", 
		    striped_token_len, striped_token);
	    /*If token has data, try to extract key and value*/
	    if ( striped_token_len > 0 ){
		/*log striped token*/
		char* token_text = calloc(striped_token_len+1, 1);
		memcpy(token_text, striped_token, striped_token_len);
		ZRT_LOG(L_SHORT, "token= '%s'", token_text);
		free(token_text);
		/*parse pair 'key=value', strip spaces */
		if ( !extract_key_value( striped_token, striped_token_len,
					 &temp_parsed[parsed_params_count].key, 
					 &temp_parsed[parsed_params_count].keylen,
					 &temp_parsed[parsed_params_count].val, 
					 &temp_parsed[parsed_params_count].vallen ) ){
                    /*one of record parameters was parsed*/
                    /*increase counter of parsed data*/
                    ++parsed_params_count;
                    /*If get waiting count of record parameters*/
                    if ( parsed_params_count == key_list->key_count ){
			(*parsed_records_count)++;
			/*parsed params count is enough to save it as single record.
			 *add it to parsed records array*/
			records = realloc(records, 
					  sizeof(struct ParsedRecord)*(*parsed_records_count));
			ZRT_LOG(L_INFO, "save record #%d", (*parsed_records_count));
			struct ParsedRecord* record = 
			    save_parsed_param_into_record(key_list,
							  temp_parsed,
							  key_list->key_count);
			ZRT_LOG(L_INFO, P_TEXT, "save record OK");
			if ( record == NULL ){
			    /*record parsing error*/
			    free(records);
			    records = NULL;
			    break;
			}
			else{
			    /*record parsed OK*/
			    records[*parsed_records_count - 1] = *record;
			}
			/* current record parsed, reset params count 
			 * to be able parse new record*/
			parsed_params_count=0;
                    }
                }
		else{
		    ZRT_LOG(L_ERROR, P_TEXT, "last token parsing error");
		    error =-1;
		}
            }
	    /*restore processing state*/
	    st = st_prev;
	    ZRT_LOG(L_INFO, P_TEXT, "restore previous parsing state");
	    /*save begin of unprocessed data to know start position of unhandled data*/
	    lex_cursor = cursor;
	    lex_length=0;
            break;
	}
        default:
            break;
        }

    }while( cursor < len );

    /*free temprorary parsing data*/
    free(temp_parsed);
    ZRT_LOG(L_INFO, P_TEXT, "return records");
    return records; /*complete if OK, or NULL if error*/
}



//end of file