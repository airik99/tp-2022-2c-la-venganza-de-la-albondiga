#include <kernel_utils.h>

config_kernel config_valores;
t_config *config;
t_log *logger;
t_queue *cola_new, *cola_ready_prioritaria, *cola_ready_segundo_nivel;
t_list *lista_colas_bloqueo;

pthread_mutex_t mx_cola_new, mx_cola_ready_prioritaria, mx_cola_ready_segunda;
sem_t sem_procesos_new, sem_procesos_ready, sem_grado_multiprogramacion, sem_page_fault;
pthread_t t_page_fault, t_largo_plazo, t_quantum, t_corto_plazo, t_manejo_consola;


char *estado_a_string[] = {"NEW", "READY", "EXEC", "BLOCKED", "EXIT"};
int conexion_cpu_dispatch;
int conexion_memoria;
int conexion_cpu_interrupt;
int socket_servidor;
int contador_pid;

void iniciar_logger() {
    logger = log_create("cfg/kernel.log", "KERNEL", true, LOG_LEVEL_INFO);
}

void cargar_configuracion() {
    config = config_create("cfg/Kernel.config");
    log_info(logger, "Arranco a leer el archivo de configuracion");
    config_valores.ip_memoria = config_get_string_value(config, "IP_MEMORIA");
    config_valores.puerto_memoria = config_get_string_value(config, "PUERTO_MEMORIA");
    config_valores.ip_cpu = config_get_string_value(config, "IP_CPU");
    config_valores.puerto_cpu_dispatch = config_get_string_value(config, "PUERTO_CPU_DISPATCH");
    config_valores.puerto_cpu_interrupt = config_get_string_value(config, "PUERTO_CPU_INTERRUPT");
    config_valores.puerto_escucha = config_get_string_value(config, "PUERTO_ESCUCHA");
    config_valores.algoritmo_planificacion = config_get_string_value(config, "ALGORITMO_PLANIFICACION");
    config_valores.grado_max_multiprogramacion = config_get_int_value(config, "GRADO_MAX_MULTIPROGRAMACION");
    config_valores.dispositivos_io = config_get_array_value(config, "DISPOSITIVOS_IO");
    config_valores.tiempos_io = config_get_array_value(config, "TIEMPOS_IO");
    config_valores.quantum_rr = config_get_int_value(config, "QUANTUM_RR");

    log_info(logger, "Termino de leer el archivo de configuracion");
}

void destruir_estructuras() {
    config_destroy(config);
    log_destroy(logger);
    string_array_destroy(config_valores.dispositivos_io);
    string_array_destroy(config_valores.tiempos_io);
}

t_pcb *crear_nuevo_pcb(t_proceso *proceso_consola, int socket) {
    t_pcb *nuevo_pcb = malloc(sizeof(t_pcb));
    nuevo_pcb->pid = contador_pid;
    contador_pid++;
    nuevo_pcb->socket_consola = socket;
    nuevo_pcb->instrucciones = proceso_consola->instrucciones;
    for (int i = 0; i < 4; i++)
        nuevo_pcb->registro[i] = 0;
    nuevo_pcb->program_counter = 0;
    nuevo_pcb->estado_actual = NEW;
    nuevo_pcb->estado_anterior = NEW;
    return nuevo_pcb;
}

void liberar_colas() {
    queue_destroy_and_destroy_elements(cola_ready_prioritaria, (void *)eliminar_pcb);
    if (es_algoritmo_FEEDBACK()) queue_destroy_and_destroy_elements(cola_ready_segundo_nivel, (void *)eliminar_pcb);
    queue_destroy_and_destroy_elements(cola_new, (void *)eliminar_pcb);
    list_destroy_and_destroy_elements(lista_colas_bloqueo, destruir_cola_bloqueo);
}

void destruir_cola_bloqueo(t_cola_bloqueo* cola){
    //no hace falta hacer free(cola->dispositivo) porque ya se hace en string_array_destroy(config_valores.dispositivos_io);
   pthread_mutex_destroy(&(cola->mx_cola_bloqueados)); 
   sem_destroy(&(cola->procesos_bloqueado));
   pthread_cancel(cola->t_bloqueo);
   queue_destroy_and_destroy_elements(cola->cola_bloqueados,destruir_solicitud_bloqueo);
   free(cola);
}

void destruir_solicitud_bloqueo(t_solicitud_io* solicitud){
    eliminar_pcb(solicitud->pcb);
    liberar_solicitud(solicitud);
}


void liberar_solicitud(t_solicitud_io* solicitud) {
    free(solicitud->dispositivo);
    free(solicitud->parametro);
    free(solicitud);
}

void eliminar_semaforos() {
    pthread_mutex_destroy(&mx_cola_new);
    pthread_mutex_destroy(&mx_cola_ready_prioritaria);
    if (es_algoritmo_FEEDBACK()) pthread_mutex_destroy(&mx_cola_ready_segunda);
    sem_destroy(&sem_grado_multiprogramacion);
    sem_destroy(&sem_procesos_new);
    sem_destroy(&sem_grado_multiprogramacion);
    
}

void actualizar_estado(t_pcb *pcb, t_estado nuevo_estado) {
    log_info(logger, "PID: <%d> - Estado Anterior: <%s> - Estado Actual: <%s>",
             pcb->pid, estado_a_string[pcb->estado_actual], estado_a_string[nuevo_estado]);
    pcb->estado_anterior = pcb->estado_actual;
    pcb->estado_actual = nuevo_estado;
}

bool es_algoritmo_FEEDBACK() {
    return strcmp(config_valores.algoritmo_planificacion, "FEEDBACK") == 0;
}
bool es_algoritmo_FIFO() {
    return strcmp(config_valores.algoritmo_planificacion, "FIFO") == 0;
}
bool es_algoritmo_RR() {
    return strcmp(config_valores.algoritmo_planificacion, "RR") == 0;
}

void loggear_colas_ready() {
    char *pids_primera_cola = string_de_pids(cola_ready_prioritaria);
    if (es_algoritmo_FEEDBACK()) {
        char *pids_segunda_cola = string_de_pids(cola_ready_segundo_nivel);
        log_info(logger, "Cola Ready <Round Robin>: [%s]", pids_primera_cola);
        log_info(logger, "Cola Ready <FIFO>: [%s]", pids_segunda_cola);
        free(pids_segunda_cola);
    } else
        log_info(logger, "Cola Ready <%s>: [%s]", config_valores.algoritmo_planificacion, pids_primera_cola);
    free(pids_primera_cola);
}

char *obtener_pid_como_string(t_pcb *pcb) {
    return string_itoa(pcb->pid);
}

char *concatenar_string_con_coma(char *string1, char *string2) {
    string_append_with_format(&string1, ", %s", string2);
    free(string2);
    return string1;
}

char *string_de_pids(t_queue *cola) {
    char *resultado;
    t_list *pid_list = list_map(cola->elements, (void *)obtener_pid_como_string);
    if (list_is_empty(pid_list))
        resultado = string_duplicate("");
    else
        resultado = list_fold1(pid_list, (void *)concatenar_string_con_coma);
    list_destroy(pid_list);
    return resultado;
}

void manejador_seniales(int senial) {
    switch (senial) {
        case SIGINT:
            log_info(logger, "Cerrando hilos");
            pthread_cancel(t_corto_plazo);
            pthread_cancel(t_manejo_consola);
            pthread_cancel(t_largo_plazo);
            break;
    }
}
