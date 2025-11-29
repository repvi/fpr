#include "lib/adc_events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "standard/time.h"
#include "driver/gptimer.h"
#include "esp_intr_alloc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "string.h"
#include "math.h"

#define DATA_QUEUE_SIZE 50
#define ADC_MAX_FILTER_SAMPLES 16
#define ADC_DEFAULT_HYSTERESIS 50
#define ADC_CALIBRATION_DEFAULT_VREF 1100
#define ADC_STATS_WINDOW_SIZE 100
#define ADC_MAX_RAW_VALUE 4095  // 12-bit ADC maximum value
#define ADC_MIN_RAW_VALUE 0     // 12-bit ADC minimum value

#define cOFF 0
#define cON  1
typedef bool adc_active_t;

typedef struct {
    gptimer_handle_t handle;      // Timer handle for audio timing
    gptimer_config_t config;
    gptimer_alarm_config_t alarm_config;
    gptimer_event_callbacks_t callbacks;
} adc_gptimer_t;

typedef struct adc_valid_channel_t {
    bool valid;
    adc_channel_t channel;
} adc_valid_channel_t;

typedef struct adc_channel_ctx_t {
    adc_oneshot_unit_handle_t adc_handle;
    adc_oneshot_chan_cfg_t chan_cfg;
    adc_oneshot_unit_init_cfg_t init_config;
    adc_channel_t adc_channel;
} adc_channel_ctx_t;

typedef struct adc_event_filter_t {
    int32_t *samples;
    uint8_t sample_count;
    uint8_t max_samples;
    uint8_t current_index;
    int64_t sum;
    bool enabled;
} adc_event_filter_t;

typedef struct adc_event_instance_t {
    char name[32];
    QueueHandle_t data_queue;
    adc_event_callback_t err_cb;
    adc_event_callback_t error_cb;
    adc_event_execute_fn_t hardware_fn;
    adc_event_type_t adc_event_type;
    void *err_cb_arg;
    void *error_cb_arg;
    void *hardware_fn_arg;
    int upper_range;
    int lower_range;
    int hysteresis;
    adc_active_t active;
    bool in_range_state;
    int last_raw_value;
    adc_event_statistics_t stats;
    adc_event_filter_t filter;
} adc_event_instance_t;

typedef struct adc_events_t {
    adc_gptimer_t gptimer;
    adc_channel_ctx_t channel_ctx;
    TaskHandle_t task_handle;
    adc_event_instance_t *virtual_channel;
    adc_cali_handle_t cali_handle;
    SemaphoreHandle_t mutex;
    int vc;
    int vs;
    int interval;
    bool calibration_enabled;
    bool running;
    uint32_t total_samples;
    uint32_t total_errors;
} adc_events_t;

static const char *TAG = "adc_events";

static esp_err_t adc_calibration_init(adc_events_handler_t handler, adc_unit_t unit, adc_atten_t atten)
{
    adc_cali_handle_t cali_handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle);
        if (ret == ESP_OK) {
            calibrated = true;
            ESP_LOGI(TAG, "ADC calibration: Curve Fitting");
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &cali_handle);
        if (ret == ESP_OK) {
            calibrated = true;
            ESP_LOGI(TAG, "ADC calibration: Line Fitting");
        }
    }
#endif

    if (calibrated) {
        handler->cali_handle = cali_handle;
        handler->calibration_enabled = true;
    } else {
        ESP_LOGW(TAG, "ADC calibration not available, using raw values");
        handler->calibration_enabled = false;
    }

    return ret;
}

static void adc_calibration_deinit(adc_events_handler_t handler)
{
    if (handler->cali_handle != NULL) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_delete_scheme_curve_fitting(handler->cali_handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_delete_scheme_line_fitting(handler->cali_handle);
#endif
        handler->cali_handle = NULL;
        handler->calibration_enabled = false;
    }
}

static int adc_raw_to_voltage(adc_events_handler_t handler, int raw)
{
    if (handler->calibration_enabled && handler->cali_handle != NULL) {
        int voltage = 0;
        if (adc_cali_raw_to_voltage(handler->cali_handle, raw, &voltage) == ESP_OK) {
            return voltage;
        }
    }
    return raw;
}

static void adc_filter_init(adc_event_filter_t *filter, uint8_t sample_count)
{
    if (sample_count > ADC_MAX_FILTER_SAMPLES) {
        sample_count = ADC_MAX_FILTER_SAMPLES;
    }
    filter->samples = heap_caps_calloc(sample_count, sizeof(int32_t), MALLOC_CAP_8BIT);
    if (filter->samples != NULL) {
        filter->max_samples = sample_count;
        filter->sample_count = 0;
        filter->current_index = 0;
        filter->sum = 0;
        filter->enabled = true;
    } else {
        filter->enabled = false;
    }
}

static void adc_filter_deinit(adc_event_filter_t *filter)
{
    if (filter->samples != NULL) {
        heap_caps_free(filter->samples);
        filter->samples = NULL;
    }
    filter->enabled = false;
}

static int32_t adc_filter_add_sample(adc_event_filter_t *filter, int32_t sample)
{
    if (!filter->enabled || filter->samples == NULL) {
        return sample;
    }

    if (filter->sample_count < filter->max_samples) {
        filter->samples[filter->sample_count] = sample;
        filter->sum += sample;
        filter->sample_count++;
        return filter->sum / filter->sample_count;
    } else {
        filter->sum -= filter->samples[filter->current_index];
        filter->samples[filter->current_index] = sample;
        filter->sum += sample;
        filter->current_index = (filter->current_index + 1) % filter->max_samples;
        return filter->sum / filter->max_samples;
    }
}

static void adc_stats_init(adc_event_statistics_t *stats)
{
    stats->min_value = ADC_MAX_RAW_VALUE;  // Start with max, will be reduced
    stats->max_value = ADC_MIN_RAW_VALUE;  // Start with min, will be increased
    stats->sum_value = 0;
    stats->sample_count = 0;
    stats->error_count = 0;
    stats->trigger_count = 0;
    stats->queue_overflow_count = 0;
    stats->last_value = 0;
}

static void adc_stats_update(adc_event_statistics_t *stats, int32_t value)
{
    if (value < stats->min_value) stats->min_value = value;
    if (value > stats->max_value) stats->max_value = value;
    stats->sum_value += value;
    stats->sample_count++;
    stats->last_value = value;
}

static adc_valid_channel_t adc_valid_channel(gpio_num_t pin)
{
    adc_valid_channel_t result = { .valid = false, .channel = ADC_CHANNEL_0 };

    if (pin >= GPIO_NUM_36) {
        result.valid = true;
        result.channel = (adc_channel_t)(pin - GPIO_NUM_36);
    }
    else if (pin >= GPIO_NUM_32) {
        result.valid = true;
        result.channel = (adc_channel_t)(pin - GPIO_NUM_32 + 4);
    }
    return result;
}

static esp_err_t adc_channel_ctx_t_set(adc_channel_ctx_t *ctx, adc_channel_t channel) 
{
    esp_err_t err;
    ctx->adc_channel = channel;
    ctx->init_config.unit_id = ADC_UNIT_1; // only ADC1 is supported
    ctx->init_config.clk_src = 0; // to be safe
    ctx->init_config.ulp_mode = ADC_ULP_MODE_DISABLE;
    err = adc_oneshot_new_unit(&ctx->init_config, &ctx->adc_handle);
    if (err != ESP_OK) {
        return err;
    }

    ctx->chan_cfg.atten = ADC_ATTEN_DB_12;
    ctx->chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;

    err = adc_oneshot_config_channel(ctx->adc_handle, ctx->adc_channel, &ctx->chan_cfg);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

adc_events_handler_t adc_events_create(gpio_num_t pin, int virtual_channels)
{
    adc_events_handler_t handler = NULL;
    adc_valid_channel_t adc_channel = adc_valid_channel(pin);
    
    if (!adc_channel.valid) {
        ESP_LOGE(TAG, "Invalid GPIO pin %d for ADC", pin);
        return NULL;
    }
    
    if (virtual_channels <= 0 || virtual_channels > 32) {
        ESP_LOGE(TAG, "Invalid virtual_channels count: %d (must be 1-32)", virtual_channels);
        return NULL;
    }
    
    int total_size = sizeof(adc_events_t) + (virtual_channels * sizeof(adc_event_instance_t));
    handler = (adc_events_handler_t)heap_caps_calloc(1, total_size, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    
    if (handler == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for ADC events handler");
        return NULL;
    }
    
    esp_err_t err = adc_channel_ctx_t_set(&handler->channel_ctx, adc_channel.channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC channel: %s", esp_err_to_name(err));
        heap_caps_free(handler);
        return NULL;
    }
    
    handler->mutex = xSemaphoreCreateMutex();
    if (handler->mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        adc_oneshot_del_unit(handler->channel_ctx.adc_handle);
        heap_caps_free(handler);
        return NULL;
    }
    
    handler->virtual_channel = (adc_event_instance_t *)((uint8_t *)handler + sizeof(adc_events_t));
    handler->vc = 0;
    handler->vs = virtual_channels;
    handler->interval = 100;
    handler->running = false;
    handler->total_samples = 0;
    handler->total_errors = 0;
    handler->cali_handle = NULL;
    handler->calibration_enabled = false;
    
    adc_calibration_init(handler, ADC_UNIT_1, ADC_ATTEN_DB_12);
    
    for (int i = 0; i < virtual_channels; i++) {
        handler->virtual_channel[i].hysteresis = ADC_DEFAULT_HYSTERESIS;
        handler->virtual_channel[i].in_range_state = false;
        handler->virtual_channel[i].last_raw_value = 0;
        handler->virtual_channel[i].error_cb = NULL;
        handler->virtual_channel[i].error_cb_arg = NULL;
        adc_stats_init(&handler->virtual_channel[i].stats);
    }
    
    ESP_LOGI(TAG, "ADC events handler created: pin=%d, virtual_channels=%d", pin, virtual_channels);
    return handler;
}

void adc_event_attach_set(adc_event_attach_t *data, const char *name, adc_event_callback_t fn, void *arg, int lower_range, int upper_range)
{
    data->err_cb = fn;
    data->hardware_fn = NULL;
    data->name = name;
    data->err_cb_arg = arg;
    data->lower_range = lower_range;
    data->upper_range = upper_range;
}

void adc_events_attach(adc_events_handler_t handler, const adc_event_attach_t *event_attach, adc_event_type_t event_type)
{
    if (handler == NULL || event_attach == NULL) {
        ESP_LOGE(TAG, "Invalid arguments to adc_events_attach");
        return;
    }
    
    if (xSemaphoreTake(handler->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return;
    }
    
    int i = handler->vc;
    if (i >= handler->vs) {
        ESP_LOGE(TAG, "No more virtual channels available (%d/%d used)", i, handler->vs);
        xSemaphoreGive(handler->mutex);
        return;
    }
    
    adc_event_instance_t *event_instance = &handler->virtual_channel[i];
    
    if (event_type == ADC_EVENT_TYPE_QUEUE) {
        event_instance->data_queue = xQueueCreate(DATA_QUEUE_SIZE, sizeof(int));
        if (event_instance->data_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create queue for virtual channel %d", i);
            xSemaphoreGive(handler->mutex);
            return;
        }
    } else {
        event_instance->data_queue = NULL;
    }
    
    if (event_attach->name) {
        strncpy(event_instance->name, event_attach->name, sizeof(event_instance->name) - 1);
        event_instance->name[sizeof(event_instance->name) - 1] = '\0';
    } else {
        snprintf(event_instance->name, sizeof(event_instance->name), "ADC_VC_%d", i);
    }
    
    event_instance->err_cb = event_attach->err_cb;
    event_instance->hardware_fn = event_attach->hardware_fn;
    event_instance->err_cb_arg = event_attach->err_cb_arg;
    event_instance->hardware_fn_arg = event_attach->hardware_fn_arg;
    event_instance->lower_range = event_attach->lower_range;
    event_instance->upper_range = event_attach->upper_range;
    event_instance->adc_event_type = event_type;
    event_instance->active = cON;
    event_instance->error_cb = NULL;
    event_instance->error_cb_arg = NULL;
    
    adc_filter_init(&event_instance->filter, 4);
    
    handler->vc++;
    xSemaphoreGive(handler->mutex);
    
    ESP_LOGI(TAG, "Attached virtual channel %d: '%s' type=%d range=[%d,%d]", 
             i, event_instance->name, event_type, event_attach->lower_range, event_attach->upper_range);
}

static esp_err_t adc_read_and_filter(adc_events_handler_t handler, adc_event_instance_t *vc_inst, int *filtered_val)
{
    const adc_channel_ctx_t channel_ctx = handler->channel_ctx;
    int raw_val;
    
    esp_err_t err = adc_oneshot_read(channel_ctx.adc_handle, channel_ctx.adc_channel, &raw_val);
    
    if (err != ESP_OK) {
        vc_inst->stats.error_count++;
        handler->total_errors++;
        
        if (vc_inst->error_cb != NULL) {
            vc_inst->error_cb(vc_inst->error_cb_arg);
        }
        
        ESP_LOGD(TAG, "ADC read error on '%s': %s", vc_inst->name, esp_err_to_name(err));
        return err;
    }
    
    handler->total_samples++;
    *filtered_val = adc_filter_add_sample(&vc_inst->filter, raw_val);
    adc_stats_update(&vc_inst->stats, *filtered_val);
    
    return ESP_OK;
}

static void adc_handle_in_range_event(adc_event_instance_t *vc_inst, int filtered_val)
{
    const int lower = vc_inst->lower_range;
    const int upper = vc_inst->upper_range;
    const int hyst = vc_inst->hysteresis;
    
    bool currently_in_range = (filtered_val >= lower && filtered_val <= upper);
    bool was_in_range = vc_inst->in_range_state;
    
    if (currently_in_range && !was_in_range) {
        if (filtered_val >= (lower + hyst) && filtered_val <= (upper - hyst)) {
            vc_inst->in_range_state = true;
            if (vc_inst->err_cb != NULL) {
                vc_inst->stats.trigger_count++;
                vc_inst->err_cb(vc_inst->err_cb_arg);
            }
        }
    } else if (!currently_in_range && was_in_range) {
        vc_inst->in_range_state = false;
    } else if (currently_in_range && was_in_range) {
        if (vc_inst->err_cb != NULL) {
            vc_inst->err_cb(vc_inst->err_cb_arg);
        }
    }
}

static void adc_handle_out_of_range_event(adc_event_instance_t *vc_inst, int filtered_val)
{
    const int lower = vc_inst->lower_range;
    const int upper = vc_inst->upper_range;
    const int hyst = vc_inst->hysteresis;
    
    bool currently_in_range = (filtered_val >= lower && filtered_val <= upper);
    bool was_in_range = vc_inst->in_range_state;
    
    if (!currently_in_range && was_in_range) {
        if (filtered_val < (lower - hyst) || filtered_val > (upper + hyst)) {
            vc_inst->in_range_state = false;
            if (vc_inst->err_cb != NULL) {
                vc_inst->stats.trigger_count++;
                vc_inst->err_cb(vc_inst->err_cb_arg);
            }
        }
    } else if (currently_in_range && !was_in_range) {
        vc_inst->in_range_state = true;
    } else if (!currently_in_range && !was_in_range) {
        if (vc_inst->err_cb != NULL) {
            vc_inst->err_cb(vc_inst->err_cb_arg);
        }
    }
}

static void adc_handle_queue_event(adc_event_instance_t *vc_inst, int filtered_val)
{
    if (vc_inst->data_queue != NULL) {
        if (xQueueSend(vc_inst->data_queue, &filtered_val, 0) != pdTRUE) {
            vc_inst->stats.queue_overflow_count++;
        }
    }
}

static void adc_handle_rising_edge_event(adc_event_instance_t *vc_inst, int filtered_val)
{
    const int lower = vc_inst->lower_range;
    
    if (vc_inst->last_raw_value < lower && filtered_val >= lower) {
        if (vc_inst->err_cb != NULL) {
            vc_inst->stats.trigger_count++;
            vc_inst->err_cb(vc_inst->err_cb_arg);
        }
    }
}

static void adc_handle_falling_edge_event(adc_event_instance_t *vc_inst, int filtered_val)
{
    const int upper = vc_inst->upper_range;
    
    if (vc_inst->last_raw_value > upper && filtered_val <= upper) {
        if (vc_inst->err_cb != NULL) {
            vc_inst->stats.trigger_count++;
            vc_inst->err_cb(vc_inst->err_cb_arg);
        }
    }
}

static void adc_handle_change_event(adc_event_instance_t *vc_inst, int filtered_val)
{
    const int hyst = vc_inst->hysteresis;
    
    if (abs(filtered_val - vc_inst->last_raw_value) > hyst) {
        if (vc_inst->err_cb != NULL) {
            vc_inst->stats.trigger_count++;
            vc_inst->err_cb(vc_inst->err_cb_arg);
        }
    }
}

static void adc_process_event(adc_event_instance_t *vc_inst, int filtered_val)
{
    switch (vc_inst->adc_event_type) {
        case ADC_EVENT_TYPE_IN_RANGE:
            adc_handle_in_range_event(vc_inst, filtered_val);
            break;
            
        case ADC_EVENT_TYPE_OUT_OF_RANGE:
            adc_handle_out_of_range_event(vc_inst, filtered_val);
            break;
            
        case ADC_EVENT_TYPE_QUEUE:
            adc_handle_queue_event(vc_inst, filtered_val);
            break;
            
        case ADC_EVENT_TYPE_RISING_EDGE:
            adc_handle_rising_edge_event(vc_inst, filtered_val);
            break;
            
        case ADC_EVENT_TYPE_FALLING_EDGE:
            adc_handle_falling_edge_event(vc_inst, filtered_val);
            break;
            
        case ADC_EVENT_TYPE_CHANGE:
            adc_handle_change_event(vc_inst, filtered_val);
            break;
            
        default:
            break;
    }
}

static void adc_process_virtual_channel(adc_events_handler_t handler, adc_event_instance_t *vc_inst)
{
    if (vc_inst->active != cON) {
        return;
    }
    
    if (vc_inst->hardware_fn != NULL) {
        vc_inst->hardware_fn(vc_inst->hardware_fn_arg);
    }
    
    int filtered_val;
    if (adc_read_and_filter(handler, vc_inst, &filtered_val) != ESP_OK) {
        return;
    }
    
    vc_inst->last_raw_value = filtered_val;
    
    adc_process_event(vc_inst, filtered_val);
}

static void adc_events_default_task(void *param)
{
    vTaskDelay(pdMS_TO_TICKS(20));
    adc_events_handler_t handler = (adc_events_handler_t)param;
    
    ESP_LOGI(TAG, "ADC events task started, interval=%d ms", handler->interval);
    
    while (handler->running) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        if (!handler->running) break;
        
        if (xSemaphoreTake(handler->mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
            continue;
        }
        
        int vc = handler->vc;
        
        for (int i = 0; i < vc; i++) {
            adc_process_virtual_channel(handler, &handler->virtual_channel[i]);
        }
        
        xSemaphoreGive(handler->mutex);
    }
    
    ESP_LOGI(TAG, "ADC events task stopped");
    vTaskDelete(NULL);
}

bool IRAM_ATTR adc_timer_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    adc_events_handler_t handler = (adc_events_handler_t)(user_ctx);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(handler->task_handle, &xHigherPriorityTaskWoken);
    return xHigherPriorityTaskWoken == pdTRUE;
}


static esp_err_t adc_events_set_timer(adc_events_handler_t handler, int interval) 
{
    esp_err_t ret;
    handler->gptimer.handle = NULL;
    handler->gptimer.config = (gptimer_config_t ){
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .flags = {
            .allow_pd = false
        },
        .resolution_hz = 1 * 1000 * 1000, // 1 MHz for microsecond precision
    };

    ret = gptimer_new_timer(&handler->gptimer.config, &handler->gptimer.handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create GPTimer: %d", ret);
        return ret;
    }

    handler->gptimer.callbacks = (gptimer_event_callbacks_t){
        .on_alarm = adc_timer_callback
    };

    ret = gptimer_register_event_callbacks(handler->gptimer.handle, &handler->gptimer.callbacks, handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GPTimer callbacks: %d", ret);
        return ret;
    }

    handler->gptimer.alarm_config = (gptimer_alarm_config_t) {
        .alarm_count = interval,
        .reload_count = 0,
        .flags = {
            .auto_reload_on_alarm = true,
        },
    };

    ret = gptimer_set_alarm_action(handler->gptimer.handle, &handler->gptimer.alarm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set GPTimer alarm action: %d", ret);
        return ret;
    }
    ret = gptimer_enable(handler->gptimer.handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable GPTimer: %d", ret);
        return ret;
    }
    ret = gptimer_start(handler->gptimer.handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start GPTimer: %d", ret);
    }
    return ESP_OK;
}

esp_err_t adc_events_start_task(adc_events_handler_t handler, int interval)
{
    if (handler == NULL) {
        ESP_LOGE(TAG, "Handler is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (handler->running) {
        ESP_LOGW(TAG, "ADC events task already running");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (interval <= 0 || interval > 10000) {
        ESP_LOGE(TAG, "Invalid interval: %d (must be 1-10000 ms)", interval);
        return ESP_ERR_INVALID_ARG;
    }
    
    handler->interval = interval;
    handler->running = true;
    
    char task_name[24];
    snprintf(task_name, sizeof(task_name), "adc_evt_%d", (int)((uintptr_t)handler & 0xFFFF));
    
    BaseType_t ret = xTaskCreate(adc_events_default_task, task_name, 3072, handler, 
                                 tskIDLE_PRIORITY + 2, &handler->task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ADC events task");
        handler->running = false;
        return ESP_FAIL;
    }
    
    esp_err_t timer_err = adc_events_set_timer(handler, interval * 1000);
    if (timer_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(timer_err));
        handler->running = false;
        vTaskDelete(handler->task_handle);
        handler->task_handle = NULL;
        return timer_err;
    }
    
    ESP_LOGI(TAG, "ADC events task started successfully, interval=%d ms", interval);
    return ESP_OK;
}

bool adc_in_range(int val, int lower_range, int upper_range)
{
    return (lower_range <= val && val <= upper_range);
}

bool adc_out_of_range(int val, int lower_range, int upper_range)
{
    return !adc_in_range(val, lower_range, upper_range);
}

bool adc_events_in_range(adc_events_handler_t handler, int val, int index)
{
    return adc_in_range(val, handler->virtual_channel[index].lower_range, handler->virtual_channel[index].upper_range);
}

bool adc_events_out_of_range(adc_events_handler_t handler, int val, int index)
{
    return adc_out_of_range(val, handler->virtual_channel[index].lower_range, handler->virtual_channel[index].upper_range);
}

int adc_events_get_value_await(adc_events_handler_t handler, int await, int index)
{
    int sample;
    if (handler->virtual_channel[index].adc_event_type == ADC_EVENT_TYPE_QUEUE && 
        xQueueReceive(handler->virtual_channel[index].data_queue, &sample, await) == pdTRUE) {
        return sample;
    }
    return -1; // Error case
}

int adc_events_attached_amount(adc_events_handler_t handler)
{
    return handler->vc;
}

int adc_events_attached_remaining(adc_events_handler_t handler)
{
    return (handler->vs - handler->vc);
}

void adc_events_attached_pause(adc_events_handler_t handler, int index)
{
    if (index >= 0 && index < handler->vc) {
        handler->virtual_channel[index].active = cOFF;
    }
}

void adc_events_attached_resume(adc_events_handler_t handler, int index)
{
    if (index >= 0 && index < handler->vc) {
        handler->virtual_channel[index].active = cON;
    }
}

esp_err_t adc_events_destroy(adc_events_handler_t handler)
{
    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Destroying ADC events handler...");
    
    if (handler->running) {
        handler->running = false;
        
        if (handler->gptimer.handle != NULL) {
            gptimer_stop(handler->gptimer.handle);
            gptimer_disable(handler->gptimer.handle);
            gptimer_del_timer(handler->gptimer.handle);
            handler->gptimer.handle = NULL;
        }
        
        if (handler->task_handle != NULL) {
            xTaskNotifyGive(handler->task_handle);
            vTaskDelay(pdMS_TO_TICKS(50));
            handler->task_handle = NULL;
        }
    }
    
    if (xSemaphoreTake(handler->mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (int i = 0; i < handler->vc; i++) {
            adc_event_instance_t *vc = &handler->virtual_channel[i];
            
            if (vc->data_queue != NULL) {
                vQueueDelete(vc->data_queue);
                vc->data_queue = NULL;
            }
            
            adc_filter_deinit(&vc->filter);
        }
        
        xSemaphoreGive(handler->mutex);
    }
    
    adc_calibration_deinit(handler);
    
    if (handler->channel_ctx.adc_handle != NULL) {
        adc_oneshot_del_unit(handler->channel_ctx.adc_handle);
        handler->channel_ctx.adc_handle = NULL;
    }
    
    if (handler->mutex != NULL) {
        vSemaphoreDelete(handler->mutex);
        handler->mutex = NULL;
    }
    
    ESP_LOGI(TAG, "Total samples: %lu, errors: %lu", handler->total_samples, handler->total_errors);
    
    heap_caps_free(handler);
    
    ESP_LOGI(TAG, "ADC events handler destroyed");
    return ESP_OK;
}

esp_err_t adc_events_get_statistics(adc_events_handler_t handler, int index, adc_event_statistics_t *stats)
{
    if (handler == NULL || stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (index < 0 || index >= handler->vc) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(handler->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    memcpy(stats, &handler->virtual_channel[index].stats, sizeof(adc_event_statistics_t));
    
    xSemaphoreGive(handler->mutex);
    
    return ESP_OK;
}

esp_err_t adc_events_reset_statistics(adc_events_handler_t handler, int index)
{
    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (index < 0 || index >= handler->vc) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(handler->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    adc_stats_init(&handler->virtual_channel[index].stats);
    
    xSemaphoreGive(handler->mutex);
    
    return ESP_OK;
}

int32_t adc_events_get_average(adc_events_handler_t handler, int index)
{
    if (handler == NULL || index < 0 || index >= handler->vc) {
        return -1;
    }
    
    if (xSemaphoreTake(handler->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return -1;
    }
    
    adc_event_statistics_t *stats = &handler->virtual_channel[index].stats;
    int32_t avg = (stats->sample_count > 0) ? (stats->sum_value / stats->sample_count) : 0;
    
    xSemaphoreGive(handler->mutex);
    
    return avg;
}

esp_err_t adc_events_set_range(adc_events_handler_t handler, int index, int lower, int upper)
{
    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (index < 0 || index >= handler->vc) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (lower > upper) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(handler->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    handler->virtual_channel[index].lower_range = lower;
    handler->virtual_channel[index].upper_range = upper;
    
    xSemaphoreGive(handler->mutex);
    
    ESP_LOGI(TAG, "Updated range for '%s': [%d, %d]", 
             handler->virtual_channel[index].name, lower, upper);
    
    return ESP_OK;
}

esp_err_t adc_events_set_hysteresis(adc_events_handler_t handler, int index, int hysteresis)
{
    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (index < 0 || index >= handler->vc) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (hysteresis < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(handler->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    handler->virtual_channel[index].hysteresis = hysteresis;
    
    xSemaphoreGive(handler->mutex);
    
    ESP_LOGI(TAG, "Updated hysteresis for '%s': %d", 
             handler->virtual_channel[index].name, hysteresis);
    
    return ESP_OK;
}

esp_err_t adc_events_set_filter(adc_events_handler_t handler, int index, uint8_t sample_count)
{
    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (index < 0 || index >= handler->vc) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (sample_count == 0 || sample_count > ADC_MAX_FILTER_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(handler->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    adc_filter_deinit(&handler->virtual_channel[index].filter);
    adc_filter_init(&handler->virtual_channel[index].filter, sample_count);
    
    xSemaphoreGive(handler->mutex);
    
    ESP_LOGI(TAG, "Updated filter for '%s': %d samples", 
             handler->virtual_channel[index].name, sample_count);
    
    return ESP_OK;
}

esp_err_t adc_events_set_error_callback(adc_events_handler_t handler, int index, 
                                        adc_event_callback_t error_cb, void *arg)
{
    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (index < 0 || index >= handler->vc) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(handler->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    handler->virtual_channel[index].error_cb = error_cb;
    handler->virtual_channel[index].error_cb_arg = arg;
    
    xSemaphoreGive(handler->mutex);
    
    return ESP_OK;
}

int adc_events_read_raw(adc_events_handler_t handler)
{
    if (handler == NULL) {
        return -1;
    }
    
    int raw_val = 0;
    esp_err_t err = adc_oneshot_read(handler->channel_ctx.adc_handle, 
                                     handler->channel_ctx.adc_channel, &raw_val);
    
    if (err != ESP_OK) {
        return -1;
    }
    
    return raw_val;
}

int adc_events_read_voltage(adc_events_handler_t handler)
{
    int raw = adc_events_read_raw(handler);
    if (raw < 0) {
        return -1;
    }
    
    return adc_raw_to_voltage(handler, raw);
}

bool adc_events_is_running(adc_events_handler_t handler)
{
    if (handler == NULL) {
        return false;
    }
    return handler->running;
}

esp_err_t adc_events_print_info(adc_events_handler_t handler)
{
    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    printf("\n========== ADC Events Handler Info ==========\n");
    printf("Running: %s\n", handler->running ? "YES" : "NO");
    printf("Interval: %d ms\n", handler->interval);
    printf("Calibration: %s\n", handler->calibration_enabled ? "ENABLED" : "DISABLED");
    printf("Virtual Channels: %d/%d\n", handler->vc, handler->vs);
    printf("Total Samples: %lu\n", handler->total_samples);
    printf("Total Errors: %lu\n", handler->total_errors);
    printf("\nVirtual Channels:\n");
    
    for (int i = 0; i < handler->vc; i++) {
        adc_event_instance_t *vc = &handler->virtual_channel[i];
        printf("  [%d] '%s':\n", i, vc->name);
        printf("      Type: %d, Active: %s\n", vc->adc_event_type, vc->active ? "YES" : "NO");
        printf("      Range: [%d, %d], Hysteresis: %d\n", 
               vc->lower_range, vc->upper_range, vc->hysteresis);
        printf("      Stats: samples=%lu, triggers=%lu, errors=%lu\n",
               vc->stats.sample_count, vc->stats.trigger_count, vc->stats.error_count);
        printf("      Values: min=%ld, max=%ld, last=%ld\n",
               vc->stats.min_value, vc->stats.max_value, vc->stats.last_value);
        if (vc->stats.sample_count > 0) {
            printf("      Average: %lld\n", vc->stats.sum_value / vc->stats.sample_count);
        }
        if (vc->filter.enabled) {
            printf("      Filter: %d samples\n", vc->filter.max_samples);
        }
    }
    
    printf("============================================\n\n");
    
    return ESP_OK;
}