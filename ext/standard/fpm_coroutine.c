#include "php.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef HAVE_TIMES
#include <sys/times.h>
#endif

#include <event2/event.h>
/* For sockaddr_in */  
#include <netinet/in.h>
#include "SAPI.h"
#include "fpm_coroutine.h"
#include "fastcgi.h"

/* use to stor coroutine context */


sapi_coroutine_context* global_coroutine_context_pool = NULL;
sapi_coroutine_context* global_coroutine_context_use = NULL;
int context_count;


/**
 * 测试输出LOG
 */
void test_log(char *text){

    FILE *pfile;
    size_t result;
    pfile=fopen("/tmp/fpmlog.txt","a+");

    int lsize=strlen(text);//获取文件长度

    result=fwrite(text,sizeof(char),lsize,pfile);//将pfile中内容读入pread指向内存中
    fclose(pfile);
}

/**
 * 注册libevent
 */
int regist_event(int fcgi_fd,void (*do_accept())){

    evutil_socket_t listener;
    struct sockaddr_in sin;
    struct event_base *base;
    struct event *listener_event;
    base = event_base_new();//初始化libevent
    if (!base)  
        return false; /*XXXerr*/  
    sin.sin_family = AF_INET;  
    sin.sin_addr.s_addr = 0;//本机  
    sin.sin_port = htons(9002); 
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (bind(listener, (struct sockaddr*)&sin, sizeof(sin)) < 0)  
    {  
        php_printf("bind");  
        return false;  
　　 }

    if (listen(listener, 16)<0)  
　　 {  
　　     php_printf("listen");  
　　     return false;  
　　 }





    



    //set coroutineinfo
    SG(coroutine_info).base = base;
    SG(coroutine_info).fcgi_fd = listener;

    char a[200];
    sprintf(a,"========= libevent base loop start ---fcgi_fd:%d ===== \n",listener);
    SG(coroutine_info).test_log(a);

    listener_event = event_new(base, listener, EV_READ|EV_PERSIST, do_accept, (void*)base);
    evutil_make_socket_nonblocking(listener);
    /* 添加事件 */  
    event_add(listener_event, NULL);
    // event_base_dispatch(base);
    event_base_loop(base,0);




    // init_coroutine_info();

    // struct event_base *base;
    // struct event *listener_event;
    // base = event_base_new();//初始化libevent
    // if (!base)  
    //     return false; //libevent 初始化失败  

    // //set coroutineinfo
    // SG(coroutine_info).base = base;
    // SG(coroutine_info).fcgi_fd = fcgi_fd;

    // char a[200];
    // sprintf(a,"========= libevent base loop start ---fcgi_fd:%d ===== \n",fcgi_fd);
    // SG(coroutine_info).test_log(a);

    // listener_event = event_new(base, fcgi_fd, EV_READ|EV_PERSIST, do_accept, base);
    // evutil_make_socket_nonblocking(fcgi_fd);

    // /* 添加事件 */  
    // event_add(listener_event, NULL);
    // // event_base_dispatch(base);
    // event_base_loop(base,0);
    // SG(coroutine_info).test_log("========= libevent base loop done ===== \n");

    return true;
}


//将Context中的内容载入全局变量
void load_coroutine_context(sapi_coroutine_context *context){

    SG(coroutine_info).init_request((void *)context->request);//=====tmp


    // php_request_startup();

    SG(coroutine_info).fpm_request_executing();

    SG(coroutine_info).context = context;//全局当前context指针

    EG(vm_stack) = context->vm_stack;
    EG(vm_stack_top) = context->vm_stack_top;
    EG(vm_stack_end) = context->vm_stack_end;

    // SG(server_context) = (void *)context->request;//load request
    EG(current_execute_data) = context->prev_execute_data;


    
    SG(sapi_headers) = context->sapi_headers;



    // SG(sapi_started) = 1;


    // SG(request_info) = context->request_info;

    // EG(symbol_table) = *context->execute_data->symbol_table;

    // EG(symbol_table) = context->symbol_table;

    // zend_hash_copy(&EG(symbol_table),&context->symbol_table,NULL);

}

//将全局变量中的数据载入Context
void write_coroutine_context(sapi_coroutine_context *context){
    context->vm_stack = EG(vm_stack);
    context->vm_stack_top = EG(vm_stack_top);
    context->vm_stack_end = EG(vm_stack_end);

    //todo 需要研究一下scoreboard，是否需要将里面的部分变量写入context

    context->sapi_headers = SG(sapi_headers);
    // context->request_info = SG(request_info);


    char a[200];
    sprintf(a,"write_coroutine_context   execute_data_ptr:%d,prev_execute_data:%d\n",context->execute_data,context->prev_execute_data);
    test_log(a);



    // *context->execute_data->symbol_table = EG(symbol_table);

    // context->symbol_table = EG(symbol_table);

    // zend_hash_copy(&context->symbol_table,&EG(symbol_table),NULL);


}

void resume_coroutine_context(sapi_coroutine_context* context){


    set_force_thread_id(context->thread_id);
    tsrm_set_interpreter_context(get_tsrm_tls_entry(context->thread_id));


    int r = setjmp(*context->buf_ptr);//yield之后的代码段，设置起始标记
    if(r == CORO_DEFAULT){//继续

        zend_vm_stack_free_args(context->prev_execute_data);
        zend_vm_stack_free_call_frame(context->prev_execute_data);

        load_coroutine_context(context);
        EG(current_execute_data)->opline++;

        zend_execute_ex(EG(current_execute_data));
        context->coro_state = CORO_END;

        char a[200];
        sprintf(a,"write_coroutine_context   execute_data_ptr:%d,prev_execute_data:%d\n",context->execute_data,context->prev_execute_data);
        test_log(a);

        zend_exception_restore();
        zend_try_exception_handler();
        if (EG(exception)) {
            zend_exception_error(EG(exception), E_ERROR);
        }

#if HAVE_BROKEN_GETCWD
        if ((int)*context->old_cwd_fd != -1) {
            fchdir(*context->old_cwd_fd);
            close(*context->old_cwd_fd);
        }
#else
        SG(coroutine_info).free_old_cwd(context->old_cwd,context->use_heap);
#endif

        //from php_execute_script_coro ,处理异常
        if (EG(exception)) {
            zend_try {
                zend_exception_error(EG(exception), E_ERROR);
            } zend_end_try();
        }

        SG(coroutine_info).close_request();
        free_coroutine_context(SG(coroutine_info).context);

    }else{
        test_log("resume code yield \n");
    }
}

void yield_coroutine_context(){

    sapi_coroutine_context* context = SG(coroutine_info).context;
    context->coro_state = CORO_YIELD;

    context->prev_execute_data = EG(current_execute_data)->prev_execute_data;
    write_coroutine_context(SG(coroutine_info).context);

    longjmp(*context->buf_ptr,CORO_YIELD);
}

/**
 * 释放上下文  todo 内存泄漏，需要进一步处理
 */
void release_coroutine_context(sapi_coroutine_context* context){
    return;

    if(SG(coroutine_info).context_count>0){

        test_log("free === 1 ===\n");

        SG(coroutine_info).context_count--;
        test_log("free === 1.1 ===\n");
        //unlink
        context->prev->next = context->next;
        context->next->prev = context->prev;
        test_log("free === 1.2 ===\n");
        //todo free all data
        char a[200];

        sprintf(a,"free === 1.3 === context->buf_ptr:%d,context->req_ptr:%d,*context->buf_ptr:%d,*context->req_ptr:%d   ===\n",context->buf_ptr,context->req_ptr,*context->buf_ptr,*context->req_ptr);
        test_log(a);

        // efree(context->buf_ptr);
        // efree(context->req_ptr);
        test_log("free === 1.4 ===\n");
        // context->buf_ptr = NULL;
        // context->req_ptr = NULL;

        test_log("free === 2 ===\n");
        zend_vm_stack_free_call_frame(context->execute_data); //释放execute_data:销毁所有的PHP变量
        context->execute_data = NULL;
        test_log("free === 2.1 ===\n");
        // efree(context->func_cache);
        // context->func_cache = NULL;


        test_log("free === 3 ===\n");

        destroy_op_array(context->op_array);
        efree_size(context->op_array, sizeof(zend_op_array));

        test_log("free === 4 ===\n");

        test_log("free === 5 ===\n");
        // efree(context);
        // context = NULL;
        test_log("free === 6 ===\n");
        if(SG(coroutine_info).context_count == 0){
            SG(coroutine_info).context = NULL;
        }
    }
}

void free_coroutine_context(sapi_coroutine_context* context){
    //先清理关系
    if(context->prev && context->next){
        context->prev->next = context->next;
        context->next->prev = context->prev;
        context->prev = NULL;
        context->next = NULL;
    }else if(context->prev){//最后一个
        context->prev->next = NULL;
        context->prev = NULL;
    }else if(context->next){//第一个
        global_coroutine_context_use = context->next;
        context->next->prev = NULL;
        context->next = NULL;
    }else{
        global_coroutine_context_use = NULL;
    }

    //加入新关系
    if(global_coroutine_context_pool){
        context->next = global_coroutine_context_pool;
        global_coroutine_context_pool->prev = context;
        context->prev = NULL;
        global_coroutine_context_pool = context;
    }else{
        global_coroutine_context_pool = context;
        context->prev = NULL;
        context->next = NULL;
    }

    context_count++;
}

void init_coroutine_set_request(sapi_coroutine_context* context,fcgi_request *request){
    context->request = request;
    SG(coroutine_info).context = context;
}

/**
 * 切换协程
 */
sapi_coroutine_context* use_coroutine_context(){
    if(context_count>0){
        sapi_coroutine_context* result = global_coroutine_context_pool;

        if(global_coroutine_context_pool && global_coroutine_context_pool->next){
            global_coroutine_context_pool = global_coroutine_context_pool->next;
            global_coroutine_context_pool->prev = NULL;
        }else{
            global_coroutine_context_pool = NULL;
        }

        if(global_coroutine_context_use){
            result->next = global_coroutine_context_use;
            result->prev = NULL;
            global_coroutine_context_use->prev = result;
            global_coroutine_context_use = result;
        }else{
            global_coroutine_context_use = result;
            global_coroutine_context_use->next = NULL;
            global_coroutine_context_use->prev = NULL;
        }

        context_count--;


        set_force_thread_id(result->thread_id);
        tsrm_set_interpreter_context(get_tsrm_tls_entry(result->thread_id));

        return result;
    }else{
        return NULL;
    }
}


/**
 * 初始化上下文
 * todo 上下文池化,不池化，会内存泄漏，100多个开始崩溃
 * 在这个函数执行之后，会适用 load_coroutine_context write_coroutine_context将context 中保存的信息导入导出
 */
void init_coroutine_context(void* tsrm_context,THREAD_T idx){
    //初始化context 上下文
    sapi_coroutine_context *context = malloc(sizeof(sapi_coroutine_context));
    context->coro_state = CORO_DEFAULT;
    context->func_cache = malloc(sizeof(zend_fcall_info_cache));
    context->request = NULL;
    context->buf_ptr = malloc(sizeof(jmp_buf));
    context->req_ptr = malloc(sizeof(jmp_buf));
    context->tsrm_context = tsrm_context;
    context->thread_id = idx;
    context->next = NULL;
    context->prev = NULL;

    //context加入链表
    if(global_coroutine_context_pool == NULL){
        global_coroutine_context_pool = context;
    }else{
        global_coroutine_context_pool->prev = context;
        context->next = global_coroutine_context_pool;
        global_coroutine_context_pool = context;
    }
    context_count++;
}

void init_coroutine_static(){
    global_coroutine_context_pool = NULL;
    global_coroutine_context_use = NULL;
    context_count = 0;
}

void init_coroutine_info(){
    SG(coroutine_info).base = NULL;
    SG(coroutine_info).fcgi_fd = NULL;
    SG(coroutine_info).context_count = &context_count;
    SG(coroutine_info).context = NULL;
    SG(coroutine_info).test_log = test_log;
    SG(coroutine_info).yield_coroutine_context = yield_coroutine_context;
    SG(coroutine_info).resume_coroutine_context = resume_coroutine_context;

    SG(coroutine_info).context_pool = &global_coroutine_context_pool;
    SG(coroutine_info).context_use = &global_coroutine_context_use;

}