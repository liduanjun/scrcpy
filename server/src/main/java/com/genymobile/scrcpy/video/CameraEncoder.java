package com.genymobile.scrcpy.video;

import com.genymobile.scrcpy.CodecOption;
import com.genymobile.scrcpy.ConfigurationException;
import com.genymobile.scrcpy.Ln;
import com.genymobile.scrcpy.LogUtils;
import com.genymobile.scrcpy.Size;
import com.genymobile.scrcpy.Streamer;
import com.genymobile.scrcpy.Workarounds;

import android.annotation.SuppressLint;
import android.annotation.TargetApi;
import android.graphics.Rect;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CaptureFailure;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.MediaCodec;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;

import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;

public class CameraEncoder extends SurfaceEncoder {

    private final String cameraId;
    private final CameraPosition cameraPosition;
    private final Handler cameraHandler;

    private int maxSize;
    private String actualCameraId;
    private CameraDevice cameraDevice;

    public CameraEncoder(int maxSize, String cameraId, CameraPosition cameraPosition, Streamer streamer,
                         int videoBitRate, int maxFps, List<CodecOption> codecOptions, String encoderName, boolean downsizeOnError) {
        super(streamer, videoBitRate, maxFps, codecOptions, encoderName, downsizeOnError);

        this.maxSize = maxSize;
        this.cameraId = cameraId;
        this.cameraPosition = cameraPosition;

        HandlerThread cameraThread = new HandlerThread("camera");
        cameraThread.start();
        cameraHandler = new Handler(cameraThread.getLooper());
    }

    @SuppressLint("MissingPermission")
    @TargetApi(Build.VERSION_CODES.N)
    private CameraDevice openCamera(String id)
            throws CameraAccessException, InterruptedException {
        Ln.v("Open Camera: " + id);

        CompletableFuture<CameraDevice> future = new CompletableFuture<>();
        Workarounds.getCameraManager().openCamera(id, new CameraDevice.StateCallback() {
            @Override
            public void onOpened(CameraDevice camera) {
                Ln.v("Open Camera Success");
                future.complete(camera);
            }

            @Override
            public void onDisconnected(CameraDevice camera) {
            }

            @Override
            public void onError(CameraDevice camera, int error) {
                int cameraAccessExceptionErrorCode;
                switch (error) {
                    case CameraDevice.StateCallback.ERROR_CAMERA_IN_USE:
                        cameraAccessExceptionErrorCode = CameraAccessException.CAMERA_IN_USE;
                        break;
                    case CameraDevice.StateCallback.ERROR_MAX_CAMERAS_IN_USE:
                        cameraAccessExceptionErrorCode = CameraAccessException.MAX_CAMERAS_IN_USE;
                        break;
                    case CameraDevice.StateCallback.ERROR_CAMERA_DISABLED:
                        cameraAccessExceptionErrorCode = CameraAccessException.CAMERA_DISABLED;
                        break;
                    case CameraDevice.StateCallback.ERROR_CAMERA_DEVICE:
                    case CameraDevice.StateCallback.ERROR_CAMERA_SERVICE:
                    default:
                        cameraAccessExceptionErrorCode = CameraAccessException.CAMERA_ERROR;
                        break;
                }
                future.completeExceptionally(new CameraAccessException(cameraAccessExceptionErrorCode));
            }
        }, cameraHandler);

        try {
            return future.get();
        } catch (ExecutionException e) {
            throw (CameraAccessException) e.getCause();
        }
    }

    @TargetApi(Build.VERSION_CODES.N)
    private CameraCaptureSession createCaptureSession(CameraDevice camera, Surface surface)
            throws CameraAccessException, InterruptedException {
        Ln.v("Create Capture Session");

        CompletableFuture<CameraCaptureSession> future = new CompletableFuture<>();
        camera.createCaptureSession(Collections.singletonList(surface), new CameraCaptureSession.StateCallback() {
            @Override
            public void onConfigured(CameraCaptureSession session) {
                Ln.v("Create Capture Session Success");
                future.complete(session);
            }

            @Override
            public void onConfigureFailed(CameraCaptureSession session) {
                future.completeExceptionally(new CameraAccessException(CameraAccessException.CAMERA_ERROR));
            }
        }, cameraHandler);

        try {
            return future.get();
        } catch (ExecutionException e) {
            throw (CameraAccessException) e.getCause();
        }
    }

    @TargetApi(Build.VERSION_CODES.N)
    private void setRepeatingRequest(CameraCaptureSession session, CaptureRequest request)
            throws CameraAccessException, InterruptedException {
        Ln.v("Set Repeating Request");

        CompletableFuture<Void> future = new CompletableFuture<>();
        session.setRepeatingRequest(request, new CameraCaptureSession.CaptureCallback() {
            @Override
            public void onCaptureStarted(CameraCaptureSession session, CaptureRequest request,
                                         long timestamp, long frameNumber) {
                future.complete(null);
            }

            @Override
            public void onCaptureFailed(CameraCaptureSession session, CaptureRequest request,
                                        CaptureFailure failure) {
                future.completeExceptionally(new CameraAccessException(CameraAccessException.CAMERA_ERROR));
            }
        }, cameraHandler);

        try {
            future.get();
        } catch (ExecutionException e) {
            throw (CameraAccessException) e.getCause();
        }
    }

    @Override
    protected void initialize() throws ConfigurationException {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            Ln.e("Camera mirroring is not support before Android 12");
            throw new ConfigurationException("Camera mirroring is not supported");
        }
    }

    @TargetApi(Build.VERSION_CODES.N)
    @Override
    protected Size getSize() throws ConfigurationException {
        try {
            if (cameraId != null) {
                if (!cameraPosition.matches(cameraId)) {
                    Ln.e(String.format("--camera=%s doesn't match --camera-postion=%s", cameraId,
                            cameraPosition.getName()));
                    throw new ConfigurationException("--camera doesn't match --camera-position");
                }
                actualCameraId = cameraId;
            } else {
                actualCameraId = null;
                String[] cameraIds = Workarounds.getCameraManager().getCameraIdList();
                for (String id : cameraIds) {
                    if (cameraPosition.matches(id)) {
                        actualCameraId = id;
                        break;
                    }
                }
                if (actualCameraId == null) {
                    Ln.e("--camera-postion doesn't match any camera");
                    throw new ConfigurationException("--camera-position doesn't match any camera");
                }
            }

            CameraCharacteristics characteristics = Workarounds.getCameraManager()
                    .getCameraCharacteristics(actualCameraId);
            Rect sensorSize = characteristics.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
            float aspectRatio = (float) sensorSize.width() / sensorSize.height();

            StreamConfigurationMap map = characteristics
                    .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            android.util.Size[] sizes = Arrays.stream(map.getOutputSizes(MediaCodec.class))
                    .filter(it -> Math.abs((float) it.getWidth() / it.getHeight() - aspectRatio) <= 0.1f)
                    .sorted(Comparator.comparing(android.util.Size::getWidth).reversed())
                    .toArray(android.util.Size[]::new);

            android.util.Size selectedSize = null;
            if (maxSize == 0) {
                selectedSize = sizes[0];
            } else {
                for (android.util.Size size : sizes) {
                    if (size.getWidth() < maxSize && size.getHeight() < maxSize) {
                        selectedSize = size;
                        break;
                    }
                }
                if (selectedSize == null) {
                    selectedSize = sizes[sizes.length - 1];
                }
            }

            return new Size(selectedSize.getWidth(), selectedSize.getHeight());
        } catch (IllegalArgumentException e) {
            Ln.e("Camera " + cameraId + " not found\n" + LogUtils.buildCameraListMessage());
            throw new ConfigurationException("Unknown camera id: " + cameraId);
        } catch (CameraAccessException e) {
            throw new RuntimeException(e);
        }
    }

    @Override
    protected void setSize(int size) {
        maxSize = size;
    }

    private void setSurfaceInternal(Surface surface) throws CameraAccessException {
        try {
            cameraDevice = openCamera(actualCameraId);
            CameraCaptureSession session = createCaptureSession(cameraDevice, surface);
            CaptureRequest.Builder requestBuilder = cameraDevice
                    .createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
            requestBuilder.addTarget(surface);
            CaptureRequest request = requestBuilder.build();
            setRepeatingRequest(session, request);
        } catch (CameraAccessException e) {
            throw e;
        } catch (Exception e) {
            throw new RuntimeException("Can't access camera", e);
        }
    }

    @Override
    protected void setSurface(Surface surface) {
        try {
            setSurfaceInternal(surface);
        } catch (CameraAccessException e) {
            throw new RuntimeException(e);
        }
    }

    @Override
    protected void dispose() {
        if (cameraDevice != null) {
            cameraDevice.close();
        }
    }
}
