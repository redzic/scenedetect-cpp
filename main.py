import numpy


def compute_frame_average(frame: numpy.ndarray) -> float:
    """Computes the average pixel value/intensity for all pixels in a frame.

    The value is computed by adding up the 8-bit R, G, and B values for
    each pixel, and dividing by the number of pixels multiplied by 3.

    Arguments:
        frame: Frame representing the RGB pixels to average.

    Returns:
        Average pixel intensity across all 3 channels of `frame`
    """
    num_pixel_values = float(frame.shape[0] * frame.shape[1] * frame.shape[2])
    avg_pixel_value = numpy.sum(frame[:, :, :]) / num_pixel_values
    return avg_pixel_value
