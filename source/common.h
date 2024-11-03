#pragma once

class SignalSmoother
{
  public:
    SignalSmoother() {}
    inline double process(double in)
    {
        double result = in + m_slope * (m_history - in);
        m_history = result;
        return result;
    }
    void setSlope(double x) { m_slope = x; }
    double getSlope() const { return m_slope; }
    void setValueImmediate(double x) { m_history = x; }

  private:
    float m_history = 0.0f;
    float m_slope = 0.999f;
};
